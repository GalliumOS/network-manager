/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright 2014-2016 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-exported-object.h"

#include <stdarg.h>
#include <string.h>

#include "nm-bus-manager.h"

#if NM_MORE_ASSERTS >= 2
#define _ASSERT_NO_EARLY_EXPORT
#endif

static gboolean quitting = FALSE;

G_DEFINE_ABSTRACT_TYPE (NMExportedObject, nm_exported_object, G_TYPE_DBUS_OBJECT_SKELETON);

typedef struct {
	GDBusInterfaceSkeleton *interface;
	guint property_changed_signal_id;
} InterfaceData;

typedef struct {
	NMBusManager *bus_mgr;
	char *path;

	GHashTable *pending_notifies;

	InterfaceData *interfaces;
	guint num_interfaces;

	guint notify_idle_id;

#ifdef _ASSERT_NO_EARLY_EXPORT
	bool _constructed:1;
#endif
} NMExportedObjectPrivate;

#define NM_EXPORTED_OBJECT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_EXPORTED_OBJECT, NMExportedObjectPrivate))

typedef struct {
	GHashTable *properties;
	GSList *skeleton_types;
	GArray *methods;
} NMExportedObjectClassInfo;

GQuark nm_exported_object_class_info_quark (void);
G_DEFINE_QUARK (NMExportedObjectClassInfo, nm_exported_object_class_info)

/*****************************************************************************/

#define _NMLOG_PREFIX_NAME                "exported-object"
#define _NMLOG_DOMAIN                     LOGD_CORE

#define _NMLOG(level, ...) \
    nm_log (level, _NMLOG_DOMAIN, \
            "%s[%p]: " _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
            _NMLOG_PREFIX_NAME, (self) \
            _NM_UTILS_MACRO_REST (__VA_ARGS__))

/*****************************************************************************/

/* "AddConnectionUnsaved" -> "handle-add-connection-unsaved" */
char *
nm_exported_object_skeletonify_method_name (const char *dbus_method_name)
{
	GString *out;
	const char *p;

	out = g_string_new ("handle");
	for (p = dbus_method_name; *p; p++) {
		if (g_ascii_isupper (*p) || p == dbus_method_name) {
			g_string_append_c (out, '-');
			g_string_append_c (out, g_ascii_tolower (*p));
		} else
			g_string_append_c (out, *p);
	}

	return g_string_free (out, FALSE);
}

/* "can-modify" -> "CanModify" */
static char *
dbusify_name (const char *gobject_name)
{
	GString *out;
	const char *p;
	gboolean capitalize = TRUE;

	out = g_string_new ("");
	for (p = gobject_name; *p; p++) {
		if (capitalize) {
			g_string_append_c (out, g_ascii_toupper (*p));
			capitalize = FALSE;
		} else if (*p == '-')
			capitalize = TRUE;
		else
			g_string_append_c (out, *p);
	}

	return g_string_free (out, FALSE);
}

/* "can_modify" -> "can-modify". Returns %NULL if @gobject_name contains no underscores */
static char *
hyphenify_name (const char *gobject_name)
{
	char *hyphen_name, *p;

	if (!strchr (gobject_name, '_'))
		return NULL;

	hyphen_name = g_strdup (gobject_name);
	for (p = hyphen_name; *p; p++) {
		if (*p == '_')
			*p = '-';
	}
	return hyphen_name;
}

/* Called when an #NMExportedObject emits a signal that corresponds to a D-Bus
 * signal, and re-emits that signal on the correct skeleton object as well.
 */
static gboolean
nm_exported_object_signal_hook (GSignalInvocationHint *ihint,
                                guint                  n_param_values,
                                const GValue          *param_values,
                                gpointer               data)
{
	NMExportedObject *self = g_value_get_object (&param_values[0]);
	NMExportedObjectPrivate *priv;
	GSignalQuery *signal_info = data;
	GDBusInterfaceSkeleton *interface = NULL;
	GValue *dbus_param_values;
	guint i;

	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);
	if (!priv->path)
		return TRUE;

	for (i = 0; i < priv->num_interfaces; i++) {
		InterfaceData *ifdata = &priv->interfaces[i];

		if (g_type_is_a (G_OBJECT_TYPE (ifdata->interface), signal_info->itype)) {
			interface = ifdata->interface;
			break;
		}
	}
	g_return_val_if_fail (interface != NULL, TRUE);

	dbus_param_values = g_newa (GValue, n_param_values);
	memset (dbus_param_values, 0, sizeof (GValue) * n_param_values);
	g_value_init (&dbus_param_values[0], G_OBJECT_TYPE (interface));
	g_value_set_object (&dbus_param_values[0], interface);
	for (i = 1; i < n_param_values; i++) {
		if (g_type_is_a (param_values[i].g_type, NM_TYPE_EXPORTED_OBJECT)) {
			NMExportedObject *arg = g_value_get_object (&param_values[i]);

			g_value_init (&dbus_param_values[i], G_TYPE_STRING);
			if (arg && nm_exported_object_is_exported (arg))
				g_value_set_string (&dbus_param_values[i], nm_exported_object_get_path (arg));
			else
				g_value_set_string (&dbus_param_values[i], "/");
		} else {
			g_value_init (&dbus_param_values[i], param_values[i].g_type);
			g_value_copy (&param_values[i], &dbus_param_values[i]);
		}
	}

	g_signal_emitv (dbus_param_values, signal_info->signal_id, 0, NULL);

	for (i = 0; i < n_param_values; i++)
		g_value_unset (&dbus_param_values[i]);

	return TRUE;
}

/**
 * nm_exported_object_class_add_interface:
 * @object_class: an #NMExportedObjectClass
 * @dbus_skeleton_type: the type of the #GDBusInterfaceSkeleton to add
 * @...: method name / handler pairs, %NULL-terminated
 *
 * Adds @dbus_skeleton_type to the list of D-Bus interfaces implemented by
 * @object_class. Instances of @object_class will automatically have a skeleton
 * of that type created, which will be exported when you call
 * nm_exported_object_export().
 *
 * The skeleton's properties will be initialized from the #NMExportedObject's,
 * and bidirectional bindings will be set up between them. When exported
 * properties change, both the org.freedesktop.DBus.Properties.PropertiesChanged
 * signal and the traditional NetworkManager PropertiesChanged signal will be
 * emitted.
 *
 * When a signal is emitted on an #NMExportedObject that has the same name as a
 * signal on @dbus_skeleton_type, it will automatically be emitted on the
 * skeleton as well; #NMExportedObject arguments in the signal will be converted
 * to D-Bus object paths in the skeleton signal.
 *
 * The arguments after @dbus_skeleton_type are pairs of D-Bus method names (in
 * CamelCase), and the corresponding handlers for them (which must have the same
 * prototype as the corresponding "handle-..." signal on @dbus_skeleton_type,
 * except with no return value, and with the first argument being an object of
 * @object_class's type, not of @dbus_skeleton_type).
 *
 * It is a programmer error if:
 *   - @object_class does not define a property of the same name and type as
 *     each of @dbus_skeleton_type's properties.
 *   - @object_class does not define a signal with the same name and arguments
 *     as each of @dbus_skeleton_type's signals.
 *   - the list of method names includes any names that do not correspond to
 *     "handle-" signals on @dbus_skeleton_type.
 *   - the list of method names does not include every method defined by
 *     @dbus_skeleton_type.
 */
void
nm_exported_object_class_add_interface (NMExportedObjectClass *object_class,
                                        GType                  dbus_skeleton_type,
                                        ...)
{
	NMExportedObjectClassInfo *classinfo;
	NMExportedObjectDBusMethodImpl method;
	va_list ap;
	const char *method_name;
	GCallback impl;
	gs_free GType *interfaces = NULL;
	guint n_interfaces;
	guint n_signals, n_method_signals;
	guint object_signal_id;
	GSignalQuery query;
	int i, s;
	GObjectClass *dbus_object_class;
	gs_free GParamSpec **dbus_properties = NULL;
	GParamSpec *object_property;
	guint n_dbus_properties;

	g_return_if_fail (NM_IS_EXPORTED_OBJECT_CLASS (object_class));
	g_return_if_fail (g_type_is_a (dbus_skeleton_type, G_TYPE_DBUS_INTERFACE_SKELETON));

	classinfo = g_slice_new (NMExportedObjectClassInfo);
	classinfo->skeleton_types = NULL;
	classinfo->methods = g_array_new (FALSE, FALSE, sizeof (NMExportedObjectDBusMethodImpl));
	classinfo->properties = g_hash_table_new (g_str_hash, g_str_equal);
	g_type_set_qdata (G_TYPE_FROM_CLASS (object_class),
	                  nm_exported_object_class_info_quark (), classinfo);

	classinfo->skeleton_types = g_slist_prepend (classinfo->skeleton_types,
	                                             GSIZE_TO_POINTER (dbus_skeleton_type));

	/* Ensure @dbus_skeleton_type's class_init has run, so its signals/properties
	 * will be defined.
	 */
	dbus_object_class = g_type_class_ref (dbus_skeleton_type);

	/* Add method implementations from the varargs */
	va_start (ap, dbus_skeleton_type);
	while ((method_name = va_arg (ap, const char *)) && (impl = va_arg (ap, GCallback))) {
		method.dbus_skeleton_type = dbus_skeleton_type;
		method.method_name = nm_exported_object_skeletonify_method_name (method_name);
		g_assert (g_signal_lookup (method.method_name, dbus_skeleton_type) != 0);
		method.impl = impl;

		g_array_append_val (classinfo->methods, method);
	}
	va_end (ap);

	/* Properties */
	dbus_properties = g_object_class_list_properties (dbus_object_class, &n_dbus_properties);
	for (i = 0; i < n_dbus_properties; i++) {
		char *hyphen_name;

		if (g_str_has_prefix (dbus_properties[i]->name, "g-"))
			continue;

		object_property = g_object_class_find_property (G_OBJECT_CLASS (object_class),
		                                                dbus_properties[i]->name);
		g_assert (object_property != NULL);
		g_assert (object_property->value_type == dbus_properties[i]->value_type);

		g_assert (!g_hash_table_contains (classinfo->properties, dbus_properties[i]->name));
		g_hash_table_insert (classinfo->properties,
		                     g_strdup (dbus_properties[i]->name),
		                     dbusify_name (dbus_properties[i]->name));
		hyphen_name = hyphenify_name (dbus_properties[i]->name);
		if (hyphen_name) {
			g_assert (!g_hash_table_contains (classinfo->properties, hyphen_name));
			g_hash_table_insert (classinfo->properties,
			                     hyphen_name,
			                     dbusify_name (dbus_properties[i]->name));
		}
	}

	/* Signals. Unlike g_object_class_list_properties(), g_signal_list_ids() is
	 * "shallow", so we need to query each implemented gdbus-generated interface
	 * separately.
	 */
	interfaces = g_type_interfaces (dbus_skeleton_type, &n_interfaces);
	n_method_signals = 0;
	for (i = 0; i < n_interfaces; i++) {
		gs_free guint *dbus_signals = NULL;

		dbus_signals = g_signal_list_ids (interfaces[i], &n_signals);
		for (s = 0; s < n_signals; s++) {
			g_signal_query (dbus_signals[s], &query);

			/* PropertiesChanged is handled specially */
			if (!strcmp (query.signal_name, "properties-changed"))
				continue;

			if (g_str_has_prefix (query.signal_name, "handle-")) {
				n_method_signals++;
				continue;
			}

			object_signal_id = g_signal_lookup (query.signal_name, G_TYPE_FROM_CLASS (object_class));
			g_assert (object_signal_id != 0);

			g_signal_add_emission_hook (object_signal_id, 0,
			                            nm_exported_object_signal_hook,
			                            g_memdup (&query, sizeof (query)),
			                            g_free);
		}
	}

	g_assert_cmpint (n_method_signals, ==, classinfo->methods->len);

	g_type_class_unref (dbus_object_class);
}

/*****************************************************************************/

/* "meta-marshaller" that receives the skeleton "handle-foo" signal, replaces
 * the skeleton object with an #NMExportedObject in the parameters, drops the
 * user_data parameter, and adds a "TRUE" return value (indicating to gdbus that
 * the signal was handled).
 */
static void
nm_exported_object_meta_marshal (GClosure *closure, GValue *return_value,
                                 guint n_param_values, const GValue *param_values,
                                 gpointer invocation_hint, gpointer marshal_data)
{
	GValue *local_param_values;

	local_param_values = g_new0 (GValue, n_param_values);
	g_value_init (&local_param_values[0], G_TYPE_POINTER);
	g_value_set_pointer (&local_param_values[0], closure->data);
	memcpy (local_param_values + 1, param_values + 1, (n_param_values - 1) * sizeof (GValue));

	g_cclosure_marshal_generic (closure, NULL,
	                            n_param_values, local_param_values,
	                            invocation_hint,
	                            ((GCClosure *)closure)->callback);
	g_value_set_boolean (return_value, TRUE);

	g_value_unset (&local_param_values[0]);
	g_free (local_param_values);
}

GQuark _skeleton_data_quark (void);
G_DEFINE_QUARK (skeleton-data, _skeleton_data);

typedef struct {
	GBinding **prop_bindings;
	gulong *method_signals;
} SkeletonData;

GDBusInterfaceSkeleton *
nm_exported_object_skeleton_create (GType dbus_skeleton_type,
                                    GObjectClass *object_class,
                                    const NMExportedObjectDBusMethodImpl *methods,
                                    guint methods_len,
                                    GObject *target)
{
	GDBusInterfaceSkeleton *interface;
	gs_free GParamSpec **properties = NULL;
	SkeletonData *skeleton_data;
	guint n_properties;
	guint i, j;

	interface = G_DBUS_INTERFACE_SKELETON (g_object_new (dbus_skeleton_type, NULL));

	skeleton_data = g_slice_new (SkeletonData);

	/* Bind properties */
	properties = g_object_class_list_properties (G_OBJECT_GET_CLASS (interface), &n_properties);
	skeleton_data->prop_bindings = g_new (GBinding *, n_properties + 1);
	for (i = 0, j = 0; i < n_properties; i++) {
		GParamSpec *nm_property;
		GBindingFlags flags;
		GBinding *prop_binding;

		nm_property = g_object_class_find_property (object_class, properties[i]->name);
		if (!nm_property)
			continue;

		flags = G_BINDING_SYNC_CREATE;
		if (   (nm_property->flags & G_PARAM_WRITABLE)
			&& !(nm_property->flags & G_PARAM_CONSTRUCT_ONLY))
			flags |= G_BINDING_BIDIRECTIONAL;
		prop_binding = g_object_bind_property (target, properties[i]->name,
		                                       interface, properties[i]->name,
		                                       flags);
		if (prop_binding)
			skeleton_data->prop_bindings[j++] = prop_binding;
	}
	skeleton_data->prop_bindings[j++] = NULL;

	/* Bind methods */
	skeleton_data->method_signals = g_new (gulong, methods_len + 1);
	for (i = 0, j = 0; i < methods_len; i++) {
		const NMExportedObjectDBusMethodImpl *method = &methods[i];
		GClosure *closure;
		gulong method_signal;

		/* ignore methods that are for a different skeleton-type. */
		if (   method->dbus_skeleton_type
		    && method->dbus_skeleton_type != dbus_skeleton_type)
			continue;

		closure = g_cclosure_new_swap (method->impl, target, NULL);
		g_closure_set_meta_marshal (closure, NULL, nm_exported_object_meta_marshal);
		method_signal = g_signal_connect_closure (interface, method->method_name, closure, FALSE);

		if (method_signal != 0)
			skeleton_data->method_signals[j++] = method_signal;
	}
	skeleton_data->method_signals[j++] = 0;

	g_object_set_qdata ((GObject *) interface, _skeleton_data_quark (), skeleton_data);

	return interface;
}

static void
nm_exported_object_create_skeletons (NMExportedObject *self,
                                     GType object_type)
{
	NMExportedObjectPrivate *priv;
	GObjectClass *object_class;
	NMExportedObjectClassInfo *classinfo;
	GSList *iter;
	const NMExportedObjectDBusMethodImpl *methods;
	guint i, methods_len;
	guint num_interfaces;
	InterfaceData *interfaces;

	classinfo = g_type_get_qdata (object_type, nm_exported_object_class_info_quark ());
	if (!classinfo)
		return;

	object_class = g_type_class_peek (object_type);
	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	methods = classinfo->methods->len ? &g_array_index (classinfo->methods, NMExportedObjectDBusMethodImpl, 0) : NULL;
	methods_len = classinfo->methods->len;

	num_interfaces = g_slist_length (classinfo->skeleton_types);
	g_return_if_fail (num_interfaces > 0);

	interfaces = g_slice_alloc (sizeof (InterfaceData) * (num_interfaces + priv->num_interfaces));

	for (i = num_interfaces, iter = classinfo->skeleton_types; iter; iter = iter->next) {
		InterfaceData *ifdata = &interfaces[--i];

		ifdata->interface = nm_exported_object_skeleton_create (GPOINTER_TO_SIZE (iter->data),
		                                                        object_class,
		                                                        methods,
		                                                        methods_len,
		                                                        (GObject *) self);
		g_dbus_object_skeleton_add_interface ((GDBusObjectSkeleton *) self, ifdata->interface);

		ifdata->property_changed_signal_id = g_signal_lookup ("properties-changed", G_OBJECT_TYPE (ifdata->interface));
	}
	nm_assert (i == 0);

	if (priv->num_interfaces > 0) {
		memcpy (&interfaces[num_interfaces], priv->interfaces, sizeof (InterfaceData) * priv->num_interfaces);
		g_slice_free1 (sizeof (InterfaceData) * priv->num_interfaces, priv->interfaces);
	}

	priv->num_interfaces = num_interfaces + priv->num_interfaces;
	priv->interfaces = interfaces;
}

void
nm_exported_object_skeleton_release (GDBusInterfaceSkeleton *interface)
{
	SkeletonData *skeleton_data;
	guint j;

	g_return_if_fail (G_IS_DBUS_INTERFACE_SKELETON (interface));

	skeleton_data = g_object_steal_qdata ((GObject *) interface, _skeleton_data_quark ());

	for (j = 0; skeleton_data->prop_bindings[j]; j++)
		g_object_unref (skeleton_data->prop_bindings[j]);
	for (j = 0; skeleton_data->method_signals[j]; j++)
		g_signal_handler_disconnect (interface, skeleton_data->method_signals[j]);

	g_free (skeleton_data->prop_bindings);
	g_free (skeleton_data->method_signals);
	g_slice_free (SkeletonData, skeleton_data);

	g_object_unref (interface);
}

static void
nm_exported_object_destroy_skeletons (NMExportedObject *self)
{
	NMExportedObjectPrivate *priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);
	guint n;

	g_return_if_fail (priv->num_interfaces > 0);
	nm_assert (priv->interfaces);

	n = priv->num_interfaces;

	while (priv->num_interfaces > 0) {
		InterfaceData *ifdata = &priv->interfaces[--priv->num_interfaces];

		g_dbus_object_skeleton_remove_interface ((GDBusObjectSkeleton *) self, ifdata->interface);
		nm_exported_object_skeleton_release (ifdata->interface);
	}

	g_slice_free1 (sizeof (InterfaceData) * n, priv->interfaces);
	priv->interfaces = NULL;
}

static char *
_create_export_path (NMExportedObjectClass *klass)
{
	const char *class_export_path, *p;
	static GHashTable *prefix_counters;
	guint *counter;

	class_export_path = klass->export_path;

	nm_assert (class_export_path);

	p = strchr (class_export_path, '%');
	if (p) {
		if (G_UNLIKELY (!prefix_counters))
			prefix_counters = g_hash_table_new (g_str_hash, g_str_equal);

		g_assert (p[1] == 'u');
		g_assert (strchr (p + 1, '%') == NULL);

		counter = g_hash_table_lookup (prefix_counters, class_export_path);
		if (!counter) {
			counter = g_slice_new0 (guint);
			g_hash_table_insert (prefix_counters, g_strdup (class_export_path), counter);
		}

		return g_strdup_printf (class_export_path, (*counter)++);
	}

	return g_strdup (class_export_path);
}

/**
 * nm_exported_object_get_path:
 * @self: an #NMExportedObject
 *
 * Gets @self's D-Bus path.
 *
 * Returns: @self's D-Bus path, or %NULL if @self is not exported.
 */
const char *
nm_exported_object_get_path (NMExportedObject *self)
{
	g_return_val_if_fail (NM_IS_EXPORTED_OBJECT (self), NULL);

	return NM_EXPORTED_OBJECT_GET_PRIVATE (self)->path;
}

/**
 * nm_exported_object_is_exported:
 * @self: an #NMExportedObject
 *
 * Checks if @self is exported
 *
 * Returns: %TRUE if @self is exported
 */
gboolean
nm_exported_object_is_exported (NMExportedObject *self)
{
	g_return_val_if_fail (NM_IS_EXPORTED_OBJECT (self), FALSE);

	return NM_EXPORTED_OBJECT_GET_PRIVATE (self)->path != NULL;
}

/**
 * nm_exported_object_export:
 * @self: an #NMExportedObject
 *
 * Exports @self on all active and future D-Bus connections.
 *
 * The path to export @self on is taken from its #NMObjectClass's %export_path
 * member. If the %export_path contains "%u", then it will be replaced with a
 * monotonically increasing integer ID (with each distinct %export_path having
 * its own counter). Otherwise, %export_path will be used literally (implying
 * that @self must be a singleton).
 *
 * Returns: the path @self was exported under
 */
const char *
nm_exported_object_export (NMExportedObject *self)
{
	NMExportedObjectPrivate *priv;
	GType type;

	g_return_val_if_fail (NM_IS_EXPORTED_OBJECT (self), NULL);
	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	g_return_val_if_fail (!priv->path, priv->path);
	g_return_val_if_fail (!priv->bus_mgr, priv->path);

#ifdef _ASSERT_NO_EARLY_EXPORT
	nm_assert (priv->_constructed);
#endif

	priv->bus_mgr = nm_bus_manager_get ();
	if (!priv->bus_mgr)
		g_return_val_if_reached (NULL);
	g_object_add_weak_pointer ((GObject *) priv->bus_mgr, (gpointer *) &priv->bus_mgr);

	type = G_OBJECT_TYPE (self);
	while (type != NM_TYPE_EXPORTED_OBJECT) {
		nm_exported_object_create_skeletons (self, type);
		type = g_type_parent (type);
	}

	priv->path = _create_export_path (NM_EXPORTED_OBJECT_GET_CLASS (self));

	_LOGT ("export: \"%s\"", priv->path);
	g_dbus_object_skeleton_set_object_path (G_DBUS_OBJECT_SKELETON (self), priv->path);

	/* Important: priv->path and priv->interfaces must not change while
	 * the object is registered. */

	nm_bus_manager_register_object (priv->bus_mgr, (GDBusObjectSkeleton *) self);

	return priv->path;
}

/**
 * nm_exported_object_unexport:
 * @self: an #NMExportedObject
 *
 * Unexports @self on all active D-Bus connections (and prevents it from being
 * auto-exported on future connections).
 */
void
nm_exported_object_unexport (NMExportedObject *self)
{
	NMExportedObjectPrivate *priv;

	g_return_if_fail (NM_IS_EXPORTED_OBJECT (self));
	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	g_return_if_fail (priv->path);

	/* Important: priv->path and priv->interfaces must not change while
	 * the object is registered. */

	_LOGT ("unexport: \"%s\"", priv->path);

	if (priv->bus_mgr) {
		nm_bus_manager_unregister_object (priv->bus_mgr, (GDBusObjectSkeleton *) self);
		g_object_remove_weak_pointer ((GObject *) priv->bus_mgr, (gpointer *) &priv->bus_mgr);
		priv->bus_mgr = NULL;
	}

	nm_exported_object_destroy_skeletons (self);

	g_dbus_object_skeleton_set_object_path ((GDBusObjectSkeleton *) self, NULL);

	g_clear_pointer (&priv->path, g_free);

	if (nm_clear_g_source (&priv->notify_idle_id)) {
		/* We had a notification queued. Since we removed all interfaces,
		 * the notification is obsolete and must be cleaned up. */
		g_hash_table_remove_all (priv->pending_notifies);
	}
}

/*****************************************************************************/

void
_nm_exported_object_clear_and_unexport (NMExportedObject **location)
{
	NMExportedObject *self;
	NMExportedObjectPrivate *priv;

	if (!location || !*location)
		return;

	self = *location;
	*location = NULL;

	g_return_if_fail (NM_IS_EXPORTED_OBJECT (self));

	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	if (priv->path)
		nm_exported_object_unexport (self);

	g_object_unref (self);
}

/*****************************************************************************/

GDBusInterfaceSkeleton *
nm_exported_object_get_interface_by_type (NMExportedObject *self, GType interface_type)
{
	NMExportedObjectPrivate *priv;
	guint i;

	g_return_val_if_fail (NM_IS_EXPORTED_OBJECT (self), NULL);

	priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	g_return_val_if_fail (priv->path, NULL);
	g_return_val_if_fail (priv->num_interfaces > 0, NULL);

	nm_assert (priv->interfaces);

	for (i = 0; i < priv->num_interfaces; i++) {
		InterfaceData *ifdata = &priv->interfaces[i];

		if (G_TYPE_CHECK_INSTANCE_TYPE (ifdata->interface, interface_type))
			return ifdata->interface;
	}
	return NULL;
}

/*****************************************************************************/

void
nm_exported_object_class_set_quitting (void)
{
	quitting = TRUE;
}

/*****************************************************************************/

typedef struct {
	const char *property_name;
	GVariant *variant;
} PendingNotifiesItem;

static int
_sort_pending_notifies (gconstpointer a, gconstpointer b, gpointer       user_data)
{
	return strcmp (((const PendingNotifiesItem *) a)->property_name,
	               ((const PendingNotifiesItem *) b)->property_name);
}

static gboolean
idle_emit_properties_changed (gpointer self)
{
	NMExportedObjectPrivate *priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);
	gs_unref_variant GVariant *variant = NULL;
	InterfaceData *ifdata = NULL;
	GHashTableIter hash_iter;
	GVariantBuilder notifies;
	guint i, n;
	PendingNotifiesItem *values;

	priv->notify_idle_id = 0;


	n = g_hash_table_size (priv->pending_notifies);
	g_return_val_if_fail (n > 0, FALSE);

	values = g_alloca (sizeof (values[0]) * n);

	i = 0;
	g_hash_table_iter_init (&hash_iter, priv->pending_notifies);
	while (g_hash_table_iter_next (&hash_iter, (gpointer) &values[i].property_name, (gpointer) &values[i].variant))
		i++;
	nm_assert (i == n);

	g_qsort_with_data (values, n, sizeof (values[0]), _sort_pending_notifies, NULL);

	g_variant_builder_init (&notifies, G_VARIANT_TYPE_VARDICT);
	for (i = 0; i < n; i++)
		g_variant_builder_add (&notifies, "{sv}", values[i].property_name, values[i].variant);
	variant = g_variant_ref_sink (g_variant_builder_end (&notifies));

	g_hash_table_remove_all (priv->pending_notifies);

	for (i = 0; i < priv->num_interfaces; i++) {
		if (priv->interfaces[i].property_changed_signal_id != 0) {
			ifdata = &priv->interfaces[i];
			break;
		}
	}
	g_return_val_if_fail (ifdata, FALSE);

	if (nm_logging_enabled (LOGL_DEBUG, LOGD_DBUS_PROPS)) {
		gs_free char *notification = g_variant_print (variant, TRUE);

		nm_log_dbg (LOGD_DBUS_PROPS, "PropertiesChanged %s %p: %s",
		            G_OBJECT_TYPE_NAME (self), self, notification);
	}

	g_signal_emit (ifdata->interface, ifdata->property_changed_signal_id, 0, variant);
	return FALSE;
}

static void
nm_exported_object_notify (GObject *object, GParamSpec *pspec)
{
	NMExportedObjectPrivate *priv = NM_EXPORTED_OBJECT_GET_PRIVATE (object);
	NMExportedObjectClassInfo *classinfo;
	GType type;
	const char *dbus_property_name = NULL;
	GValue value = G_VALUE_INIT;
	const GVariantType *vtype;
	guint i, j;

	if (priv->num_interfaces == 0)
		return;

	for (type = G_OBJECT_TYPE (object); type; type = g_type_parent (type)) {
		classinfo = g_type_get_qdata (type, nm_exported_object_class_info_quark ());
		if (!classinfo)
			continue;

		dbus_property_name = g_hash_table_lookup (classinfo->properties, pspec->name);
		if (dbus_property_name)
			break;
	}
	if (!dbus_property_name) {
		nm_log_trace (LOGD_DBUS_PROPS, "ignoring notification for prop %s on type %s",
		              pspec->name, G_OBJECT_TYPE_NAME (object));
		return;
	}

	for (i = 0; i < priv->num_interfaces; i++) {
		GDBusInterfaceSkeleton *skel = priv->interfaces[i].interface;
		GDBusInterfaceInfo *iinfo;

		iinfo = g_dbus_interface_skeleton_get_info (skel);
		for (j = 0; iinfo->properties[j]; j++) {
			if (nm_streq (iinfo->properties[j]->name, dbus_property_name)) {
				vtype = G_VARIANT_TYPE (iinfo->properties[j]->signature);
				goto vtype_found;
			}
		}
	}
	g_return_if_reached ();

vtype_found:
	g_value_init (&value, pspec->value_type);
	g_object_get_property (G_OBJECT (object), pspec->name, &value);

	/* @dbus_property_name is inside classinfo and never freed, thus we don't clone it.
	 * Also, we do a pointer, not string comparison. */
	g_hash_table_insert (priv->pending_notifies,
	                     (gpointer) dbus_property_name,
	                     g_dbus_gvalue_to_gvariant (&value, vtype));
	g_value_unset (&value);

	if (!priv->notify_idle_id)
		priv->notify_idle_id = g_idle_add (idle_emit_properties_changed, object);
}

/*****************************************************************************/

static void
nm_exported_object_init (NMExportedObject *self)
{
	NMExportedObjectPrivate *priv = NM_EXPORTED_OBJECT_GET_PRIVATE (self);

	priv->pending_notifies = g_hash_table_new_full (g_direct_hash,
	                                                g_direct_equal,
	                                                NULL,
	                                                (GDestroyNotify) g_variant_unref);
}

static void
constructed (GObject *object)
{
	NMExportedObjectClass *klass;

	G_OBJECT_CLASS (nm_exported_object_parent_class)->constructed (object);

#ifdef _ASSERT_NO_EARLY_EXPORT
	NM_EXPORTED_OBJECT_GET_PRIVATE (object)->_constructed = TRUE;
#endif

	klass = NM_EXPORTED_OBJECT_GET_CLASS (object);

	if (klass->export_on_construction)
		nm_exported_object_export ((NMExportedObject *) object);
}

static void
nm_exported_object_dispose (GObject *object)
{
	NMExportedObjectPrivate *priv = NM_EXPORTED_OBJECT_GET_PRIVATE (object);

	/* Objects should have already been unexported by their owner, unless
	 * we are quitting, where many objects stick around until exit.
	 */
	if (!quitting) {
		if (priv->path) {
			g_warn_if_reached ();
			nm_exported_object_unexport (NM_EXPORTED_OBJECT (object));
		}
	} else
		g_clear_pointer (&priv->path, g_free);

	g_clear_pointer (&priv->pending_notifies, g_hash_table_destroy);
	nm_clear_g_source (&priv->notify_idle_id);

	G_OBJECT_CLASS (nm_exported_object_parent_class)->dispose (object);
}

static void
nm_exported_object_class_init (NMExportedObjectClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMExportedObjectPrivate));

	object_class->constructed = constructed;
	object_class->notify = nm_exported_object_notify;
	object_class->dispose = nm_exported_object_dispose;
}


