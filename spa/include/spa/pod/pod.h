/* Simple Plugin API
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

#ifndef SPA_POD_H
#define SPA_POD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/type.h>

#define SPA_POD_BODY_SIZE(pod)			(((struct spa_pod*)(pod))->size)
#define SPA_POD_TYPE(pod)			(((struct spa_pod*)(pod))->type)
#define SPA_POD_SIZE(pod)			(sizeof(struct spa_pod) + SPA_POD_BODY_SIZE(pod))
#define SPA_POD_CONTENTS_SIZE(type,pod)		(SPA_POD_SIZE(pod)-sizeof(type))

#define SPA_POD_CONTENTS(type,pod)		SPA_MEMBER((pod),sizeof(type),void)
#define SPA_POD_CONTENTS_CONST(type,pod)	SPA_MEMBER((pod),sizeof(type),const void)
#define SPA_POD_BODY(pod)			SPA_MEMBER((pod),sizeof(struct spa_pod),void)
#define SPA_POD_BODY_CONST(pod)			SPA_MEMBER((pod),sizeof(struct spa_pod),const void)

struct spa_pod {
	uint32_t size;		/* size of the body */
	uint32_t type;		/* a basic id of enum spa_type */
};

#define SPA_POD_VALUE(type,pod)			(((type*)pod)->value)

struct spa_pod_bool {
	struct spa_pod pod;
	int32_t value;
	int32_t _padding;
};

struct spa_pod_id {
	struct spa_pod pod;
	uint32_t value;
	int32_t _padding;
};

struct spa_pod_int {
	struct spa_pod pod;
	int32_t value;
	int32_t _padding;
};

struct spa_pod_long {
	struct spa_pod pod;
	int64_t value;
};

struct spa_pod_float {
	struct spa_pod pod;
	float value;
	int32_t _padding;
};

struct spa_pod_double {
	struct spa_pod pod;
	double value;
};

struct spa_pod_string {
	struct spa_pod pod;
	/* value here */
};

struct spa_pod_bytes {
	struct spa_pod pod;
	/* value here */
};

struct spa_pod_rectangle {
	struct spa_pod pod;
	struct spa_rectangle value;
};

struct spa_pod_fraction {
	struct spa_pod pod;
	struct spa_fraction value;
};

struct spa_pod_bitmap {
	struct spa_pod pod;
	/* array of uint8_t follows with the bitmap */
};

#define SPA_POD_ARRAY_CHILD(arr)	(&((struct spa_pod_array*)(arr))->body.child)
#define SPA_POD_ARRAY_VALUE_TYPE(arr)	(SPA_POD_TYPE(SPA_POD_ARRAY_CHILD(arr)))
#define SPA_POD_ARRAY_VALUE_SIZE(arr)	(SPA_POD_BODY_SIZE(SPA_POD_ARRAY_CHILD(arr)))
#define SPA_POD_ARRAY_N_VALUES(arr)	((SPA_POD_BODY_SIZE(arr) - sizeof(struct spa_pod_array_body)) / SPA_POD_ARRAY_VALUE_SIZE(arr))
#define SPA_POD_ARRAY_VALUES(arr)	SPA_POD_CONTENTS(struct spa_pod_array, arr)

struct spa_pod_array_body {
	struct spa_pod child;
	/* array with elements of child.size follows */
};

struct spa_pod_array {
	struct spa_pod pod;
	struct spa_pod_array_body body;
};

#define SPA_POD_CHOICE_CHILD(choice)		(&((struct spa_pod_choice*)(choice))->body.child)
#define SPA_POD_CHOICE_TYPE(choice)		(((struct spa_pod_choice*)(choice))->body.type)
#define SPA_POD_CHOICE_FLAGS(choice)		(((struct spa_pod_choice*)(choice))->body.flags)
#define SPA_POD_CHOICE_VALUE_TYPE(choice)	(SPA_POD_TYPE(SPA_POD_CHOICE_CHILD(choice)))
#define SPA_POD_CHOICE_VALUE_SIZE(choice)	(SPA_POD_BODY_SIZE(SPA_POD_CHOICE_CHILD(choice)))
#define SPA_POD_CHOICE_N_VALUES(choice)		((SPA_POD_BODY_SIZE(choice) - sizeof(struct spa_pod_choice_body)) / SPA_POD_CHOICE_VALUE_SIZE(choice))
#define SPA_POD_CHOICE_VALUES(choice)		(SPA_POD_CONTENTS(struct spa_pod_choice, choice))

enum spa_choice_type {
	SPA_CHOICE_None,		/**< no choice, first value is current */
	SPA_CHOICE_Range,		/**< range: default, min, max */
	SPA_CHOICE_Step,		/**< range with step: default, min, max, step */
	SPA_CHOICE_Enum,		/**< list: default, alternative,...  */
	SPA_CHOICE_Flags,		/**< flags: default, possible flags,... */
};

struct spa_pod_choice_body {
	uint32_t type;			/**< type of choice, one of enum spa_choice_type */
	uint32_t flags;			/**< extra flags */
	struct spa_pod child;
	/* array with elements of child.size follows */
};

struct spa_pod_choice {
	struct spa_pod pod;
	struct spa_pod_choice_body body;
};

struct spa_pod_struct {
	struct spa_pod pod;
	/* one or more spa_pod follow */
};

#define SPA_POD_OBJECT_TYPE(obj)	(((struct spa_pod_object*)(obj))->body.type)
#define SPA_POD_OBJECT_ID(obj)		(((struct spa_pod_object*)(obj))->body.id)

#define SPA_POD_IS_OBJECT_TYPE(obj,tp)	(SPA_POD_TYPE(obj) == SPA_TYPE_Object && \
					 SPA_POD_OBJECT_TYPE(obj) == (tp))

struct spa_pod_object_body {
	uint32_t type;		/**< one of enum spa_type */
	uint32_t id;		/**< id of the object, depends on the object type */
	/* contents follow, series of spa_pod */
};

struct spa_pod_object {
	struct spa_pod pod;
	struct spa_pod_object_body body;
};

struct spa_pod_pointer_body {
	uint32_t type;		/**< pointer id, one of enum spa_type */
	uint32_t _padding;
	const void *value;
};

struct spa_pod_pointer {
	struct spa_pod pod;
	struct spa_pod_pointer_body body;
};

struct spa_pod_fd {
	struct spa_pod pod;
	int64_t value;
};

#define SPA_POD_PROP_SIZE(prop)		(sizeof(struct spa_pod_prop) + (prop)->value.size)

/* props can be inside an object */
struct spa_pod_prop {
	uint32_t key;			/**< key of property, list of valid keys depends on the
					  *  object type */
	uint32_t context;		/**< context for property */
	struct spa_pod value;
	/* value follows */
};

#define SPA_POD_CONTROL_SIZE(ev)	(sizeof(struct spa_pod_control) + (ev)->value.size)

/* controls can be inside a sequence and mark timed values */
struct spa_pod_control {
	uint32_t offset;	/**< media offset */
	uint32_t type;		/**< type of control, enum spa_control_type */
	struct spa_pod value;	/**< control value, depends on type */
	/* value contents follow */
};

struct spa_pod_sequence_body {
	uint32_t unit;
	uint32_t pad;
	/* series of struct spa_pod_control follows */
};

/** a sequence of timed controls */
struct spa_pod_sequence {
	struct spa_pod pod;
	struct spa_pod_sequence_body body;
};


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_POD_H */
