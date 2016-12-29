#include <string.h>
#include "vis-core.h"
#include "util.h"

KeyAction *vis_action_new(Vis *vis, const char *name, const char *help, KeyActionFunction *func, Arg arg) {
	KeyAction *action = calloc(1, sizeof *action);
	if (!action)
		return NULL;
	if (name && !(action->name = strdup(name)))
		goto err;
	if (help && !(action->help = strdup(help)))
		goto err;
	action->func = func;
	action->arg = arg;
	if (!array_add_ptr(&vis->actions_user, action))
		goto err;
	return action;
err:
	free((char*)action->name);
	free((char*)action->help);
	free(action);
	return NULL;
}

void vis_action_free(Vis *vis, KeyAction *action) {
	if (!action)
		return;
	size_t len = array_length(&vis->actions_user);
	for (size_t i = 0; i < len; i++) {
		if (action == array_get_ptr(&vis->actions_user, i)) {
			free((char*)action->name);
			free((char*)action->help);
			free(action);
			array_remove(&vis->actions_user, i);
			return;
		}
	}
}

KeyBinding *vis_binding_new(Vis *vis) {
	KeyBinding *binding = calloc(1, sizeof *binding);
	if (binding && array_add_ptr(&vis->bindings, binding))
		return binding;
	free(binding);
	return NULL;
}

void vis_binding_free(Vis *vis, KeyBinding *binding) {
	if (!binding)
		return;
	size_t len = array_length(&vis->bindings);
	for (size_t i = 0; i < len; i++) {
		if (binding == array_get_ptr(&vis->bindings, i)) {
			if (binding->alias)
				free((char*)binding->alias);
			if (binding->action && !binding->action->name)
				vis_action_free(vis, (KeyAction*)binding->action);
			free(binding);
			array_remove(&vis->bindings, i);
			return;
		}
	}
}

static Mode *mode_get(Vis *vis, enum VisMode mode) {
	if (mode < LENGTH(vis_modes))
		return &vis_modes[mode];
	return NULL;
}

void mode_set(Vis *vis, Mode *new_mode) {
	if (vis->mode == new_mode)
		return;
	if (vis->mode->leave)
		vis->mode->leave(vis, new_mode);
	if (vis->mode != &vis_modes[VIS_MODE_OPERATOR_PENDING])
		vis->mode_prev = vis->mode;
	vis->mode = new_mode;
	if (new_mode->enter)
		new_mode->enter(vis, vis->mode_prev);
	if (vis->event && vis->event->win_status)
		vis->event->win_status(vis, vis->win);
}

void vis_mode_switch(Vis *vis, enum VisMode mode) {
	mode_set(vis, &vis_modes[mode]);
}

enum VisMode vis_mode_get(Vis *vis) {
	return vis->mode->id;
}

static bool mode_unmap(Mode *mode, const char *key) {
	return mode && mode->bindings && map_delete(mode->bindings, key);
}

bool vis_mode_unmap(Vis *vis, enum VisMode id, const char *key) {
	return id < LENGTH(vis_modes) && mode_unmap(&vis_modes[id], key);
}

bool vis_window_mode_unmap(Win *win, enum VisMode id, const char *key) {
	return id < LENGTH(win->modes) && mode_unmap(&win->modes[id], key);
}

static bool mode_map(Vis *vis, Mode *mode, bool force, const char *key, const KeyBinding *binding) {
	if (!mode)
		return false;
	if (binding->alias && key[0] != '<' && strncmp(key, binding->alias, strlen(key)) == 0)
		return false;
	if (!mode->bindings && !(mode->bindings = map_new()))
		return false;
	if (force)
		map_prefix_delete(mode->bindings, key);
	return (strcmp(key, "<") == 0 || !map_contains(mode->bindings, key)) && map_put(mode->bindings, key, binding);
}

bool vis_mode_map(Vis *vis, enum VisMode id, bool force, const char *key, const KeyBinding *binding) {
	return id < LENGTH(vis_modes) && mode_map(vis, &vis_modes[id], force, key, binding);
}

bool vis_window_mode_map(Win *win, enum VisMode id, bool force, const char *key, const KeyBinding *binding) {
	return id < LENGTH(win->modes) && mode_map(win->vis, &win->modes[id], force, key, binding);
}

/** mode switching event handlers */

static void vis_mode_operator_input(Vis *vis, const char *str, size_t len) {
	/* invalid operator */
	vis_cancel(vis);
	mode_set(vis, vis->mode_prev);
}

static void vis_mode_visual_enter(Vis *vis, Mode *old) {
	if (!old->visual) {
		for (Cursor *c = view_cursors(vis->win->view); c; c = view_cursors_next(c))
			view_cursors_selection_start(c);
	}
}

static void vis_mode_visual_line_enter(Vis *vis, Mode *old) {
	if (!old->visual) {
		for (Cursor *c = view_cursors(vis->win->view); c; c = view_cursors_next(c))
			view_cursors_selection_start(c);
	}
	if (!vis->action.op)
		vis_motion(vis, VIS_MOVE_NOP);
}

static void vis_mode_visual_line_leave(Vis *vis, Mode *new) {
	if (!new->visual) {
		window_selection_save(vis->win);
		view_selections_clear(vis->win->view);
	} else {
		view_cursor_to(vis->win->view, view_cursor_get(vis->win->view));
	}
}

static void vis_mode_visual_leave(Vis *vis, Mode *new) {
	if (!new->visual) {
		window_selection_save(vis->win);
		view_selections_clear(vis->win->view);
	}
}

static void vis_mode_insert_enter(Vis *vis, Mode *old) {
	if (vis->win->parent)
		return;
	if (!vis->action.op) {
		action_reset(&vis->action_prev);
		vis->action_prev.op = &vis_operators[VIS_OP_MODESWITCH];
		vis->action_prev.mode = VIS_MODE_INSERT;
	}
	if (!vis->macro_operator) {
		macro_operator_record(vis);
		vis->action_prev.macro = vis->macro_operator;
	}
}

static void vis_mode_insert_leave(Vis *vis, Mode *new) {
	if (new == mode_get(vis, VIS_MODE_NORMAL)) {
		/* make sure we can recover the current state after an editing operation */
		text_snapshot(vis->win->file->text);
		macro_operator_stop(vis);
	}
}

static void vis_mode_insert_idle(Vis *vis) {
	text_snapshot(vis->win->file->text);
}

static void vis_mode_insert_input(Vis *vis, const char *str, size_t len) {
	vis_insert_key(vis, str, len);
}

static void vis_mode_replace_enter(Vis *vis, Mode *old) {
	if (vis->win->parent)
		return;
	if (!vis->action.op) {
		action_reset(&vis->action_prev);
		vis->action_prev.op = &vis_operators[VIS_OP_MODESWITCH];
		vis->action_prev.mode = VIS_MODE_REPLACE;
	}
	if (!vis->macro_operator) {
		macro_operator_record(vis);
		vis->action_prev.macro = vis->macro_operator;
	}
}

static void vis_mode_replace_leave(Vis *vis, Mode *new) {
	if (new == mode_get(vis, VIS_MODE_NORMAL)) {
		/* make sure we can recover the current state after an editing operation */
		text_snapshot(vis->win->file->text);
		macro_operator_stop(vis);
	}
}

static void vis_mode_replace_input(Vis *vis, const char *str, size_t len) {
	vis_replace_key(vis, str, len);
}

Mode vis_modes[] = {
	[VIS_MODE_OPERATOR_PENDING] = {
		.id = VIS_MODE_OPERATOR_PENDING,
		.name = "OPERATOR-PENDING",
		.input = vis_mode_operator_input,
		.help = "",
	},
	[VIS_MODE_NORMAL] = {
		.id = VIS_MODE_NORMAL,
		.name = "NORMAL",
		.help = "",
	},
	[VIS_MODE_VISUAL] = {
		.id = VIS_MODE_VISUAL,
		.name = "VISUAL",
		.status = "VISUAL",
		.help = "",
		.enter = vis_mode_visual_enter,
		.leave = vis_mode_visual_leave,
		.visual = true,
	},
	[VIS_MODE_VISUAL_LINE] = {
		.id = VIS_MODE_VISUAL_LINE,
		.name = "VISUAL LINE",
		.parent = &vis_modes[VIS_MODE_VISUAL],
		.status = "VISUAL-LINE",
		.help = "",
		.enter = vis_mode_visual_line_enter,
		.leave = vis_mode_visual_line_leave,
		.visual = true,
	},
	[VIS_MODE_INSERT] = {
		.id = VIS_MODE_INSERT,
		.name = "INSERT",
		.status = "INSERT",
		.help = "",
		.enter = vis_mode_insert_enter,
		.leave = vis_mode_insert_leave,
		.input = vis_mode_insert_input,
		.idle = vis_mode_insert_idle,
		.idle_timeout = 3,
	},
	[VIS_MODE_REPLACE] = {
		.id = VIS_MODE_REPLACE,
		.name = "REPLACE",
		.parent = &vis_modes[VIS_MODE_INSERT],
		.status = "REPLACE",
		.help = "",
		.enter = vis_mode_replace_enter,
		.leave = vis_mode_replace_leave,
		.input = vis_mode_replace_input,
		.idle = vis_mode_insert_idle,
		.idle_timeout = 3,
	},
};

