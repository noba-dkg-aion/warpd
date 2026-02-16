/*
 * warpd - A modal keyboard-driven pointing system.
 *
 * © 2019 Raheman Vaiya (see: LICENSE).
 */

#include "warpd.h"
#include <stdarg.h>
#include <stdlib.h>

struct hint *hints;
struct hint matched[MAX_HINTS];

static size_t nr_hints;
static size_t nr_matched;

char last_selected_hint[32];

enum hint_key_action {
	HINT_KEY_NONE = 0,
	HINT_KEY_EXIT,
	HINT_KEY_UNDO_ALL,
	HINT_KEY_UNDO,
	HINT_KEY_APPEND_CHAR,
	HINT_KEY_IGNORED_NON_CHAR,
	HINT_KEY_IGNORED_BUF_FULL,
};

struct hint_filter_timing {
	uint64_t match_us;
	uint64_t clear_us;
	uint64_t draw_us;
	uint64_t commit_us;
	uint64_t total_us;
};

static struct {
	int initialized;
	int enabled;
	int min_us;
	FILE *stream;
	uint64_t session_id;
	uint64_t batch_seq;
	uint64_t key_seq;
} hint_tm_state;

static const char *hint_key_action_tostr(enum hint_key_action action)
{
	switch (action) {
	case HINT_KEY_EXIT:
		return "exit";
	case HINT_KEY_UNDO_ALL:
		return "undo_all";
	case HINT_KEY_UNDO:
		return "undo";
	case HINT_KEY_APPEND_CHAR:
		return "append_char";
	case HINT_KEY_IGNORED_NON_CHAR:
		return "ignored_non_char";
	case HINT_KEY_IGNORED_BUF_FULL:
		return "ignored_buf_full";
	default:
		return "none";
	}
}

static int hint_tm_enabled()
{
	if (hint_tm_state.initialized)
		return hint_tm_state.enabled;

	hint_tm_state.initialized = 1;
	hint_tm_state.enabled = 0;
	hint_tm_state.min_us = 0;
	hint_tm_state.stream = stderr;

	const char *enabled = getenv("WARPD_HINT_TELEMETRY");
	if (!enabled || !enabled[0] || !strcmp(enabled, "0"))
		return 0;

	hint_tm_state.enabled = 1;

	const char *path = getenv("WARPD_HINT_TELEMETRY_FILE");
	if (path && path[0]) {
		FILE *f = fopen(path, "a");
		if (f)
			hint_tm_state.stream = f;
	}

	const char *min_us = getenv("WARPD_HINT_TELEMETRY_MIN_US");
	if (min_us && min_us[0]) {
		int val = atoi(min_us);
		if (val > 0)
			hint_tm_state.min_us = val;
	}

	return hint_tm_state.enabled;
}

static void hint_tm_log(const char *kind, const char *fmt, ...)
{
	if (!hint_tm_enabled())
		return;

	FILE *out = hint_tm_state.stream ? hint_tm_state.stream : stderr;
	uint64_t ts = get_time_us();
	va_list ap;

	fprintf(out, "hint_tm ts_us=%llu sid=%llu kind=%s ",
		(unsigned long long)ts,
		(unsigned long long)hint_tm_state.session_id,
		kind);

	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);

	fputc('\n', out);
	fflush(out);
}

static void filter(screen_t scr, const char *s, struct hint_filter_timing *timing)
{
	size_t i;
	uint64_t t0, t1, t2, t3, t4;

	t0 = get_time_us();

	nr_matched = 0;
	for (i = 0; i < nr_hints; i++) {
		if (strstr(hints[i].label, s) == hints[i].label)
			matched[nr_matched++] = hints[i];
	}
	t1 = get_time_us();

	platform->screen_clear(scr);
	t2 = get_time_us();
	platform->hint_draw(scr, matched, nr_matched);
	t3 = get_time_us();
	platform->commit();
	t4 = get_time_us();

	if (timing) {
		timing->match_us = t1 - t0;
		timing->clear_us = t2 - t1;
		timing->draw_us = t3 - t2;
		timing->commit_us = t4 - t3;
		timing->total_us = t4 - t0;
	}
}

static void get_hint_size(screen_t scr, int *w, int *h)
{
	int sw, sh;

	platform->screen_get_dimensions(scr, &sw, &sh);

	if (sw < sh) {
		int tmp = sw;
		sw = sh;
		sh = tmp;
	}

	*w = (sw * config_get_int("hint_size")) / 1000;
	*h = (sh * config_get_int("hint_size")) / 1000;
}

static int get_hint_label_len()
{
	int len = config_get_int("hint_label_len");

	/* Fullscreen hints are arranged in an NxN grid and need >=2 chars. */
	if (len < 2)
		return 2;
	if (len > 15)
		return 15;

	return len;
}

static size_t generate_fullscreen_hints(screen_t scr, struct hint *hints)
{
	int sw, sh;
	int w, h;
	int col, row, k;
	size_t n = 0;

	const char *chars = config_get("hint_chars");
	const int label_len = get_hint_label_len();
	get_hint_size(scr, &w, &h);
	platform->screen_get_dimensions(scr, &sw, &sh);

	const int base = strlen(chars);
	const int max_hints_config = config_get_int("hint_max_hints");
	size_t total_hint_cap = MAX_HINTS;

	if (max_hints_config >= 1 && (size_t)max_hints_config < total_hint_cap)
		total_hint_cap = (size_t)max_hints_config;

	size_t total = 1;
	int nc, nr;

	for (k = 0; k < label_len; k++) {
		if (total > total_hint_cap / (size_t)base) {
			total = total_hint_cap;
			break;
		}
		total *= (size_t)base;
	}
	if (total > total_hint_cap)
		total = total_hint_cap;

	/*
	 * Choose grid dimensions that minimize unused slots first, then keep an
	 * aspect ratio close to the screen to avoid large empty regions.
	 */
	{
		int best_nc = 1;
		int best_nr = (int)total;
		int have_best = 0;
		size_t best_waste = (size_t)best_nc * (size_t)best_nr - total;
		long long best_aspect_err =
			llabs((long long)best_nc * (long long)sh -
			      (long long)best_nr * (long long)sw);

		for (int cand_nr = 1; cand_nr <= (int)total; cand_nr++) {
			int cand_nc = (int)((total + (size_t)cand_nr - 1) /
					    (size_t)cand_nr);
			int cand_cell_w = sw / cand_nc;
			int cand_cell_h = sh / cand_nr;
			size_t cand_cap = (size_t)cand_nc * (size_t)cand_nr;
			size_t cand_waste = cand_cap - total;
			long long cand_aspect_err =
				llabs((long long)cand_nc * (long long)sh -
				      (long long)cand_nr * (long long)sw);

			/* Skip layouts that cannot fit hint boxes without overlap. */
			if (cand_cell_w < w || cand_cell_h < h)
				continue;

			if (!have_best ||
			    cand_waste < best_waste ||
			    (cand_waste == best_waste &&
			     cand_aspect_err < best_aspect_err)) {
				best_nc = cand_nc;
				best_nr = cand_nr;
				best_waste = cand_waste;
				best_aspect_err = cand_aspect_err;
				have_best = 1;
			}
		}

		if (have_best) {
			nc = best_nc;
			nr = best_nr;
		} else {
			nc = 1;
			while ((size_t)nc * (size_t)nc < total)
				nc++;
			nr = (int)((total + (size_t)nc - 1) / (size_t)nc);
		}
	}

	const int colgap = sw / nc - w;
	const int rowgap = sh / nr - h;

	const int x_offset = (sw - nc * w - (nc - 1) * colgap) / 2;
	const int y_offset = (sh - nr * h - (nr - 1) * rowgap) / 2;

	get_hint_size(scr, &w, &h);

	for (row = 0; row < nr; row++) {
		for (col = 0; col < nc; col++) {
			size_t idx = (size_t)row * (size_t)nc + (size_t)col;
			size_t tmp = idx;
			struct hint *hint;

			if (idx >= total)
				break;
			hint = &hints[n++];

			hint->x = x_offset + col * (colgap + w);
			hint->y = y_offset + row * (rowgap + h);

			hint->w = w;
			hint->h = h;

			for (k = label_len - 1; k >= 0; k--) {
				hint->label[k] = chars[tmp % (size_t)base];
				tmp /= (size_t)base;
			}
			hint->label[label_len] = 0;
		}
	}

	return n;
}

static int process_hint_event(struct input_event *ev, char *buf, int *rc,
			      int *state_changed, enum hint_key_action *action)
{
	ssize_t len = strlen(buf);
	if (action)
		*action = HINT_KEY_NONE;

	if (config_input_match(ev, "hint_exit")) {
		if (action)
			*action = HINT_KEY_EXIT;
		*rc = -1;
		return -1;
	} else if (config_input_match(ev, "hint_undo_all")) {
		if (action)
			*action = HINT_KEY_UNDO_ALL;
		if (len) {
			buf[0] = 0;
			*state_changed = 1;
		}
	} else if (config_input_match(ev, "hint_undo")) {
		if (action)
			*action = HINT_KEY_UNDO;
		if (len) {
			buf[len - 1] = 0;
			*state_changed = 1;
		}
	} else {
		const char *name = input_event_tostr(ev);

		if (!name || name[1]) {
			if (action)
				*action = HINT_KEY_IGNORED_NON_CHAR;
			return 0;
		}
		if ((size_t)len + 1 >= 32) {
			if (action)
				*action = HINT_KEY_IGNORED_BUF_FULL;
			return 0;
		}

		buf[len] = name[0];
		buf[len + 1] = 0;
		*state_changed = 1;
		if (action)
			*action = HINT_KEY_APPEND_CHAR;
	}

	return 0;
}

static int hint_selection(screen_t scr, struct hint *_hints, size_t _nr_hints)
{
	hints = _hints;
	nr_hints = _nr_hints;

	int telemetry_on = hint_tm_enabled();
	hint_tm_state.session_id++;
	hint_tm_state.batch_seq = 0;
	hint_tm_state.key_seq = 0;

	struct hint_filter_timing timing;
	filter(scr, "", telemetry_on ? &timing : NULL);

	if (telemetry_on)
		hint_tm_log("session_start",
			    "hints=%llu matched=%llu init_total_us=%llu init_match_us=%llu init_draw_us=%llu init_commit_us=%llu",
			    (unsigned long long)nr_hints,
			    (unsigned long long)nr_matched,
			    (unsigned long long)timing.total_us,
			    (unsigned long long)timing.match_us,
			    (unsigned long long)timing.draw_us,
			    (unsigned long long)timing.commit_us);

	int rc = 0;
	char buf[32] = {0};
	platform->input_grab_keyboard();

	platform->mouse_hide();

	const char *keys[] = {
		"hint_exit",
		"hint_undo_all",
		"hint_undo",
	};

	config_input_whitelist(keys, sizeof keys / sizeof keys[0]);

	while (1) {
		struct input_event *ev = platform->input_next_event(0);
		int state_changed = 0;
		int drained = 0;
		int pressed_events = 0;
		uint64_t queue_start_us = get_time_us();

		if (!ev)
			continue;

		do {
			if (ev->pressed) {
				enum hint_key_action action;
				size_t before_len = strlen(buf);
				pressed_events++;
				hint_tm_state.key_seq++;

				if (process_hint_event(ev, buf, &rc, &state_changed,
						       &action) < 0) {
					if (telemetry_on)
						hint_tm_log("outcome",
							    "reason=exit key_seq=%llu drained=%d pressed=%d queue_us=%llu",
							    (unsigned long long)hint_tm_state.key_seq,
							    drained,
							    pressed_events,
							    (unsigned long long)(get_time_us() - queue_start_us));
					goto done;
				}

				if (telemetry_on) {
					size_t after_len = strlen(buf);
					hint_tm_log("key",
						    "key_seq=%llu code=%u mods=%u action=%s before_len=%llu after_len=%llu state_changed=%d",
						    (unsigned long long)hint_tm_state.key_seq,
						    ev->code,
						    ev->mods,
						    hint_key_action_tostr(action),
						    (unsigned long long)before_len,
						    (unsigned long long)after_len,
						    state_changed);
				}
			}
			drained++;
			if (drained >= 32)
				break;

			/*
			 * Drain small bursts of queued key events and render once to
			 * avoid input-lag buildup when hint drawing is expensive.
			 */
			ev = platform->input_next_event(1);
		} while (ev);

		if (telemetry_on) {
			hint_tm_state.batch_seq++;
			hint_tm_log("batch",
				    "batch=%llu drained=%d pressed=%d queue_us=%llu state_changed=%d",
				    (unsigned long long)hint_tm_state.batch_seq,
				    drained,
				    pressed_events,
				    (unsigned long long)(get_time_us() - queue_start_us),
				    state_changed);
		}

		if (!state_changed)
			continue;

		filter(scr, buf, telemetry_on ? &timing : NULL);

		if (telemetry_on &&
		    (!hint_tm_state.min_us ||
		     timing.total_us >= (uint64_t)hint_tm_state.min_us)) {
			hint_tm_log("render",
				    "batch=%llu buf_len=%llu matched=%llu match_us=%llu clear_us=%llu draw_us=%llu commit_us=%llu total_us=%llu drained=%d pressed=%d",
				    (unsigned long long)hint_tm_state.batch_seq,
				    (unsigned long long)strlen(buf),
				    (unsigned long long)nr_matched,
				    (unsigned long long)timing.match_us,
				    (unsigned long long)timing.clear_us,
				    (unsigned long long)timing.draw_us,
				    (unsigned long long)timing.commit_us,
				    (unsigned long long)timing.total_us,
				    drained,
				    pressed_events);
		}

		if (nr_matched == 1 && !strcmp(buf, matched[0].label)) {
			int nx, ny;
			struct hint *h = &matched[0];

			platform->screen_clear(scr);

			nx = h->x + h->w / 2;
			ny = h->y + h->h / 2;

			/*
			 * Wiggle the cursor a single pixel to accommodate
			 * text selection widgets which don't like spontaneous
			 * cursor warping.
			 */
			platform->mouse_move(scr, nx+1, ny+1);

			platform->mouse_move(scr, nx, ny);
			strcpy(last_selected_hint, buf);
			if (telemetry_on)
				hint_tm_log("outcome",
					    "reason=selected label=%s",
					    buf);
			break;
		} else if (nr_matched == 0) {
			if (telemetry_on)
				hint_tm_log("outcome",
					    "reason=no_match buf_len=%llu",
					    (unsigned long long)strlen(buf));
			break;
		}
	}

done:
	platform->input_ungrab_keyboard();
	platform->screen_clear(scr);
	platform->mouse_show();

	platform->commit();
	if (telemetry_on)
		hint_tm_log("session_end",
			    "rc=%d final_buf_len=%llu selected=%s",
			    rc,
			    (unsigned long long)strlen(buf),
			    last_selected_hint[0] ? last_selected_hint : "-");
	return rc;
}

static int sift()
{
	int gap = config_get_int("hint2_gap_size");
	int hint_sz = config_get_int("hint2_size");

	const char *chars = config_get("hint2_chars");
	size_t chars_len= strlen(chars);

	int grid_sz = config_get_int("hint2_grid_size");

	int x, y;
	int sh, sw;

	int col;
	int row;
	size_t n = 0;
	screen_t scr;

	struct hint hints[MAX_HINTS];

	platform->mouse_get_position(&scr, &x, &y);
	platform->screen_get_dimensions(scr, &sw, &sh);

	gap = (gap * sh) / 1000;
	hint_sz = (hint_sz * sh) / 1000;

	x -= ((hint_sz + (gap - 1)) * grid_sz) / 2;
	y -= ((hint_sz + (gap - 1)) * grid_sz) / 2;

	for (col = 0; col < grid_sz; col++)
		for (row = 0; row < grid_sz; row++) {
			size_t idx = (row * grid_sz) + col;

			if (idx < chars_len) {
				hints[n].x = x + (hint_sz + gap) * col;
				hints[n].y = y + (hint_sz + gap) * row;

				hints[n].w = hint_sz;
				hints[n].h = hint_sz;
				hints[n].label[0] = chars[idx];
				hints[n].label[1] = 0;

				n++;
			}
	}

	return hint_selection(scr, hints, n);
}

void init_hints()
{
	platform->init_hint(config_get("hint_bgcolor"),
			    config_get("hint_fgcolor"),
			    config_get_int("hint_border_radius"),
			    config_get("hint_font"));
}

int hintspec_mode()
{
	screen_t scr;
	int sw, sh;
	int w, h;

	int n = 0;
	struct hint hints[MAX_HINTS];

	platform->mouse_get_position(&scr, NULL, NULL);
	platform->screen_get_dimensions(scr, &sw, &sh);

	get_hint_size(scr, &w, &h);

	while (scanf("%15s %d %d",
		hints[n].label,
		&hints[n].x,
		&hints[n].y) == 3) {

		hints[n].w = w;
		hints[n].h = h;
		hints[n].x -= w/2;
		hints[n].y -= h/2;

		n++;
	}

	return hint_selection(scr, hints, n);
}

int full_hint_mode(int second_pass)
{
	int mx, my;
	screen_t scr;
	struct hint hints[MAX_HINTS];

	platform->mouse_get_position(&scr, &mx, &my);
	hist_add(mx, my);

	nr_hints = generate_fullscreen_hints(scr, hints);

	if (hint_selection(scr, hints, nr_hints))
		return -1;

	if (second_pass)
		return sift();
	else
		return 0;
}

int history_hint_mode()
{
	struct hint hints[MAX_HINTS];
	struct histfile_ent *ents;
	screen_t scr;
	int w, h;
	int sw, sh;
	size_t n, i;
	const char *hint_chars;
	size_t hint_base;
	int label_len;
	size_t max_labels;

	platform->mouse_get_position(&scr, NULL, NULL);
	platform->screen_get_dimensions(scr, &sw, &sh);

	n = histfile_read(&ents);

	get_hint_size(scr, &w, &h);

	label_len = get_hint_label_len();
	hint_chars = config_get("hint_chars");
	if (!hint_chars || !hint_chars[0])
		hint_chars = "abcdefghijklmnopqrstuvwxyz";
	hint_base = strlen(hint_chars);
	if (!hint_base)
		hint_base = 1;

	max_labels = 1;
	for (size_t k = 0; k < (size_t)label_len; k++) {
		if (max_labels > MAX_HINTS / hint_base) {
			max_labels = MAX_HINTS;
			break;
		}
		max_labels *= hint_base;
	}
	if (max_labels > MAX_HINTS)
		max_labels = MAX_HINTS;
	if (!max_labels)
		max_labels = 1;

	if (n > max_labels)
		n = max_labels;

	for (i = 0; i < n; i++) {
		hints[i].w = w;
		hints[i].h = h;

		hints[i].x = ents[i].x - w/2;
		hints[i].y = ents[i].y - h/2;

		{
			size_t tmp = i;
			for (int k = label_len - 1; k >= 0; k--) {
				hints[i].label[k] = hint_chars[tmp % hint_base];
				tmp /= hint_base;
			}
			hints[i].label[label_len] = 0;
		}
	}

	return hint_selection(scr, hints, n);
}
