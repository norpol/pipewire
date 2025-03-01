/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdarg.h>

#include "pipewire/array.h"
#include "pipewire/utils.h"
#include "pipewire/properties.h"

/** \cond */
struct properties {
	struct pw_properties this;

	struct pw_array items;
};
/** \endcond */

static int add_func(struct pw_properties *this, char *key, char *value)
{
	struct spa_dict_item *item;
	struct properties *impl = SPA_CONTAINER_OF(this, struct properties, this);

	item = pw_array_add(&impl->items, sizeof(struct spa_dict_item));
	if (item == NULL)
		return -errno;

	item->key = key;
	item->value = value;

	this->dict.items = impl->items.data;
	this->dict.n_items++;
	return 0;
}

static void clear_item(struct spa_dict_item *item)
{
	free((char *) item->key);
	free((char *) item->value);
}

static int find_index(const struct pw_properties *this, const char *key)
{
	uint32_t i, len = this->dict.n_items;
	for (i = 0; i < len; i++) {
		if (strcmp(this->dict.items[i].key, key) == 0)
			return i;
	}
	return -1;
}

static struct properties *properties_new(int prealloc)
{
	struct properties *impl;

	impl = calloc(1, sizeof(struct properties));
	if (impl == NULL)
		return NULL;

	pw_array_init(&impl->items, 16);
	pw_array_ensure_size(&impl->items, sizeof(struct spa_dict_item) * prealloc);

	return impl;
}

/** Make a new properties object
 *
 * \param key a first key
 * \param ... value and more keys NULL terminated
 * \return a newly allocated properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_new(const char *key, ...)
{
	struct properties *impl;
	va_list varargs;
	const char *value;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	va_start(varargs, key);
	while (key != NULL) {
		value = va_arg(varargs, char *);
		if (value && key[0])
			add_func(&impl->this, strdup(key), strdup(value));
		key = va_arg(varargs, char *);
	}
	va_end(varargs);

	return &impl->this;
}

/** Make a new properties object from the given dictionary
 *
 * \param dict a dictionary. keys and values are copied
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_new_dict(const struct spa_dict *dict)
{
	uint32_t i;
	struct properties *impl;

	impl = properties_new(SPA_ROUND_UP_N(dict->n_items, 16));
	if (impl == NULL)
		return NULL;

	for (i = 0; i < dict->n_items; i++) {
		const struct spa_dict_item *it = &dict->items[i];
		if (it->key != NULL && it->key[0] && it->value != NULL)
			add_func(&impl->this, strdup(it->key),
				 strdup(it->value));
	}

	return &impl->this;
}

/** Make a new properties object from the given str
 *
 * \a str should be a whitespace separated list of key=value
 * strings.
 *
 * \param args a property description
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *
pw_properties_new_string(const char *str)
{

	struct properties *impl;
        const char *state = NULL, *s = NULL;
	size_t len;
	int res;

	impl = properties_new(16);
	if (impl == NULL)
		return NULL;

	s = pw_split_walk(str, " \t\n\r", &len, &state);
	while (s) {
		char *val, *eq;

		if ((val = strndup(s, len)) == NULL) {
			res = -errno;
			goto no_mem;
		}

		eq = strchr(val, '=');
		if (eq && eq != val) {
			*eq = '\0';
			add_func(&impl->this, val, strdup(eq+1));
		}
		s = pw_split_walk(str, " \t\n\r", &len, &state);
	}
	return &impl->this;

no_mem:
	pw_properties_free(&impl->this);
	errno = -res;
	return NULL;
}

/** Copy a properties object
 *
 * \param properties properties to copy
 * \return a new properties object
 *
 * \memberof pw_properties
 */
SPA_EXPORT
struct pw_properties *pw_properties_copy(const struct pw_properties *properties)
{
	return pw_properties_new_dict(&properties->dict);
}

/** Copy multiple keys from one property to another
 *
 * \param src properties to copy from
 * \param dst properties to copy to
 * \param keys a NULL terminated list of keys to copy
 * \return the number of keys changed in \a dest
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_copy_keys(const struct pw_properties *src,
		struct pw_properties *dst, const char *keys[])
{
	int i, changed = 0;
	const char *str;

	for (i = 0; keys[i]; i++) {
		if ((str = pw_properties_get(src, keys[i])) != NULL)
			changed += pw_properties_set(dst, keys[i], str);
	}
	return changed;
}

/** Clear a properties object
 *
 * \param properties properties to clear
 *
 * \memberof pw_properties
 */
SPA_EXPORT
void pw_properties_clear(struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	struct spa_dict_item *item;

	pw_array_for_each(item, &impl->items)
		clear_item(item);
	pw_array_reset(&impl->items);
	properties->dict.n_items = 0;
}

/** Update properties
 *
 * \param props properties to update
 * \param dict new properties
 * \return the number of changed properties
 *
 * The properties in \a props are updated with \a dict. Keys in \a dict
 * with NULL values are removed from \a props.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_update(struct pw_properties *props,
		         const struct spa_dict *dict)
{
	uint32_t i;
	int changed = 0;

	for (i = 0; i < dict->n_items; i++)
		changed += pw_properties_set(props, dict->items[i].key, dict->items[i].value);

	return changed;
}

/** Free a properties object
 *
 * \param properties the properties to free
 *
 * \memberof pw_properties
 */
SPA_EXPORT
void pw_properties_free(struct pw_properties *properties)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	pw_properties_clear(properties);
	pw_array_clear(&impl->items);
	free(impl);
}

static int do_replace(struct pw_properties *properties, const char *key, char *value, bool copy)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index;

	if (key == NULL || key[0] == 0)
		return 0;

	index = find_index(properties, key);

	if (index == -1) {
		if (value == NULL)
			return 0;
		add_func(properties, strdup(key), copy ? strdup(value) : value);
	} else {
		struct spa_dict_item *item =
		    pw_array_get_unchecked(&impl->items, index, struct spa_dict_item);

		if (value && strcmp(item->value, value) == 0) {
			if (!copy)
				free(value);
			return 0;
		}

		if (value == NULL) {
			struct spa_dict_item *last = pw_array_get_unchecked(&impl->items,
						     pw_array_get_len(&impl->items, struct spa_dict_item) - 1,
						     struct spa_dict_item);
			clear_item(item);
			item->key = last->key;
			item->value = last->value;
			impl->items.size -= sizeof(struct spa_dict_item);
			properties->dict.n_items--;
		} else {
			free((char *) item->value);
			item->value = copy ? strdup(value) : value;
		}
	}
	return 1;
}

/** Set a property value
 *
 * \param properties the properties to change
 * \param key a key
 * \param value a value or NULL to remove the key
 * \return 1 if the properties were changed. 0 if nothing was changed because
 *  the property already existed with the same value or because the key to remove
 *  did not exist.
 *
 * Set the property in \a properties with \a key to \a value. Any previous value
 * of \a key will be overwritten. When \a value is NULL, the key will be
 * removed.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_set(struct pw_properties *properties, const char *key, const char *value)
{
	return do_replace(properties, key, (char*)value, true);
}

SPA_EXPORT
int pw_properties_setva(struct pw_properties *properties,
		   const char *key, const char *format, va_list args)
{
	char *value;
	vasprintf(&value, format, args);
	return do_replace(properties, key, value, false);
}

/** Set a property value by format
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \param format a value
 * \param ... extra arguments
 *
 * Set the property in \a properties with \a key to the value in printf style \a format
 * Any previous value of \a key will be overwritten.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
int pw_properties_setf(struct pw_properties *properties, const char *key, const char *format, ...)
{
	int res;
	va_list varargs;

	va_start(varargs, format);
	res = pw_properties_setva(properties, key, format, varargs);
	va_end(varargs);

	return res;
}

/** Get a property
 *
 * \param properties a \ref pw_properties
 * \param key a key
 * \return the property for \a key or NULL when the key was not found
 *
 * Get the property in \a properties with \a key.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
const char *pw_properties_get(const struct pw_properties *properties, const char *key)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	int index = find_index(properties, key);

	if (index == -1)
		return NULL;

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->value;
}

/** Iterate property values
 *
 * \param properties a \ref pw_properties
 * \param state state
 * \return The next key or NULL when there are no more keys to iterate.
 *
 * Iterate over \a properties, returning each key in turn. \a state should point
 * to a pointer holding NULL to get the first element and will be updated
 * after each iteration. When NULL is returned, all elements have been
 * iterated.
 *
 * \memberof pw_properties
 */
SPA_EXPORT
const char *pw_properties_iterate(const struct pw_properties *properties, void **state)
{
	struct properties *impl = SPA_CONTAINER_OF(properties, struct properties, this);
	uint32_t index;

	if (*state == NULL)
		index = 0;
	else
		index = SPA_PTR_TO_INT(*state);

	if (!pw_array_check_index(&impl->items, index, struct spa_dict_item))
		 return NULL;

	*state = SPA_INT_TO_PTR(index + 1);

	return pw_array_get_unchecked(&impl->items, index, struct spa_dict_item)->key;
}
