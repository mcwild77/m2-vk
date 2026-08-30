// Analog stick evenness tester — SDL2, terminal UI.
//
// Reads the pad through SDL2, which is the same path RetroArch uses, so what this
// prints is what the emulator sees. Raw joystick axes: no mapping, no deadzone.
//
//   ./devnotes/tools/build-sticktest.sh && ./devnotes/tools/sticktest
//
// Roll each stick slowly around its full rim a few times, then let it centre.

#include <SDL.h>       // sdl2-config puts .../include/SDL2 on the include path
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define MAX_AXES   16
#define NBINS      72          // polar envelope sectors, 5 degrees each
#define GW         31          // ascii grid width  (chars are ~2:1, so W ~= 2*H)
#define GH         15

static volatile sig_atomic_t s_quit = 0;
static void on_sigint(int sig) { (void)sig; s_quit = 1; }

typedef struct {
	double now, min, max, rest;
	int    rest_init;
	unsigned char seen[8192];   // bitmap over the 65536 possible int16 values
	long   nsamples, pinned;   // pinned = samples sitting on the rail
} axis_t;

typedef struct {
	double env[NBINS];
	long   visits[NBINS];
	double maxr;
	double px, py;       // previous position, for segment fill
	int    has_prev;
} stick_t;

static axis_t  g_axis[MAX_AXES];
static stick_t g_stick[MAX_AXES / 2];
static int     g_naxes;
static FILE   *g_log;
static char    g_logpath[1024];

static void axis_reset(void)
{
	memset(g_axis, 0, sizeof(g_axis));
	memset(g_stick, 0, sizeof(g_stick));
	for (int i = 0; i < MAX_AXES; i++) { g_axis[i].min = 1e9; g_axis[i].max = -1e9; }
}

static void env_hit(stick_t *s, double x, double y)
{
	double r = hypot(x, y);
	if (r <= 0.15) return;
	double ang = atan2(y, x);
	if (ang < 0) ang += 2.0 * M_PI;
	int b = (int)(ang / (2.0 * M_PI) * NBINS);
	if (b >= NBINS) b = NBINS - 1;
	s->visits[b]++;
	if (r > s->env[b]) s->env[b] = r;
	if (r > s->maxr) s->maxr = r;
}

static int distinct_and_step(const axis_t *a, int *out_step)
{
	int count = 0, prev = -1, step = 65536;
	for (int v = 0; v < 65536; v++) {
		if (!(a->seen[v >> 3] & (1u << (v & 7)))) continue;
		count++;
		if (prev >= 0 && v - prev < step) step = v - prev;
		prev = v;
	}
	*out_step = (count > 1) ? step : 0;
	return count;
}

// A trigger or an unused axis reports two values and pairing it with its neighbour
// produces a meaningless "stick". Require both axes to genuinely vary.
static int is_real_stick(int i)
{
	int st;
	return distinct_and_step(&g_axis[i * 2], &st) > 8 &&
	       distinct_and_step(&g_axis[i * 2 + 1], &st) > 8;
}

// ---------------------------------------------------------------- ascii grid

static void grid_build(char g[GH][GW], const stick_t *s, double x, double y)
{
	const int cx = (GW - 1) / 2, cy = (GH - 1) / 2;
	memset(g, ' ', GH * GW);

	for (int r = 0; r < GH; r++) g[r][cx] = '|';
	for (int c = 0; c < GW; c++) g[cy][c] = '-';
	g[cy][cx] = '+';

	for (int i = 0; i < 180; i++) {                    // unit-circle reference
		double a = i / 180.0 * 2.0 * M_PI;
		int px = cx + (int)lround(cos(a) * cx);
		int py = cy + (int)lround(sin(a) * cy);
		if (px >= 0 && px < GW && py >= 0 && py < GH && g[py][px] == ' ') g[py][px] = '.';
	}

	for (int b = 0; b < NBINS; b++) {                  // measured envelope
		if (s->env[b] <= 0.0) continue;
		double a = (b + 0.5) / NBINS * 2.0 * M_PI;
		int px = cx + (int)lround(cos(a) * s->env[b] * cx);
		int py = cy + (int)lround(sin(a) * s->env[b] * cy);
		if (px >= 0 && px < GW && py >= 0 && py < GH) g[py][px] = '#';
	}

	int px = cx + (int)lround(x * cx), py = cy + (int)lround(y * cy);
	if (px >= 0 && px < GW && py >= 0 && py < GH) g[py][px] = '@';
}

static void grid_print_row(const char g[GH][GW], int row)
{
	for (int c = 0; c < GW; c++) {
		char ch = g[row][c];
		switch (ch) {
		case '#': fputs("\033[33m#\033[0m", stdout); break;
		case '@': fputs("\033[97;1m@\033[0m", stdout); break;
		case '.': fputs("\033[90m.\033[0m", stdout); break;
		case '|': case '-': case '+': printf("\033[90m%c\033[0m", ch); break;
		default:  putchar(' ');
		}
	}
}

// ---------------------------------------------------------------- reporting

static void bar(double v)
{
	const int half = 18;
	int n = (int)lround(fabs(v) * half);
	if (n > half) n = half;
	fputs("[", stdout);
	for (int i = 0; i < half; i++) putchar(v < 0 && i >= half - n ? '=' : ' ');
	fputs("\033[90m|\033[0m", stdout);
	for (int i = 0; i < half; i++) putchar(v > 0 && i < n ? '=' : ' ');
	fputs("]", stdout);
}

static const char *grade(double q, double good, double ok)
{
	return q >= good ? "\033[32m" : q >= ok ? "\033[33m" : "\033[31m";
}

static void report_stick(int i)
{
	const stick_t *s = &g_stick[i];
	const axis_t *ax = &g_axis[i * 2], *ay = &g_axis[i * 2 + 1];

	// A sector the stick merely crossed near the centre is not evidence about the
	// rim. Only sectors taken past half the observed reach count as swept.
	double mx = 0.0;
	for (int b = 0; b < NBINS; b++) if (s->env[b] > mx) mx = s->env[b];

	int swept = 0;
	double mn = 1e9, sum = 0.0;
	for (int b = 0; b < NBINS; b++) {
		if (s->env[b] < mx * 0.5) continue;
		swept++; sum += s->env[b];
		if (s->env[b] < mn) mn = s->env[b];
	}

	printf("  stick %d (a%d/a%d)  ", i, i * 2, i * 2 + 1);
	if (swept < NBINS * 8 / 10) {
		printf("\033[90mkeep rolling, hard against the gate — %d/%d sectors swept\033[0m\033[K\n",
		       swept, NBINS);
		printf("\033[K\n\033[K\n");
		return;
	}

	double mean = sum / swept, sd = 0.0;
	for (int b = 0; b < NBINS; b++)
		if (s->env[b] >= mx * 0.5) sd += (s->env[b] - mean) * (s->env[b] - mean);
	sd = sqrt(sd / swept);

	double round_q = mn / mx;
	const char *word = round_q >= 0.92 ? "EVEN" : round_q >= 0.80 ? "slightly uneven" : "UNEVEN";

	int flat = 0;
	for (int b = 0; b < NBINS; b++)
		if (s->env[b] >= mx * 0.5 && s->env[b] < mx * 0.85) flat++;

	double xl = fabs(ax->min), xr = fabs(ax->max);
	double yl = fabs(ay->min), yr = fabs(ay->max);
	double qx = (xl > 0 && xr > 0) ? fmin(xl, xr) / fmax(xl, xr) : 0.0;
	double qy = (yl > 0 && yr > 0) ? fmin(yl, yr) / fmax(yl, yr) : 0.0;
	double drift = hypot(ax->rest, ay->rest);

	printf("roundness %s%.3f  %s\033[0m  \033[90mmin %.3f max %.3f mean %.3f sd %.3f  "
	       "%d/%d swept, %d low\033[0m\033[K\n",
	       grade(round_q, 0.92, 0.80), round_q, word, mn, mx, mean, sd, swept, NBINS, flat);
	printf("      reach   X %s%+.3f / %+.3f (%.1f%% matched)\033[0m   "
	       "Y %s%+.3f / %+.3f (%.1f%% matched)\033[0m\033[K\n",
	       grade(qx, 0.95, 0.88), ax->min, ax->max, qx * 100.0,
	       grade(qy, 0.95, 0.88), ay->min, ay->max, qy * 100.0);
	printf("      centre  %sdrift %.4f\033[0m  \033[90m(rest x %+.4f  y %+.4f)\033[0m\033[K\n",
	       grade(1.0 - drift, 0.97, 0.92), drift, ax->rest, ay->rest);
}

// ---------------------------------------------------------------- log file

// Sits next to the binary unless a path is given on the command line.
static void log_open(const char *argv0, const char *override, const char *name,
                     int nbtn, int nhat, int is_ctrl)
{
	if (override) {
		snprintf(g_logpath, sizeof(g_logpath), "%s", override);
	} else {
		const char *slash = strrchr(argv0, '/');
		if (slash) snprintf(g_logpath, sizeof(g_logpath), "%.*s/sticktest.log",
		                    (int)(slash - argv0), argv0);
		else       snprintf(g_logpath, sizeof(g_logpath), "sticktest.log");
	}

	g_log = fopen(g_logpath, "w");
	if (!g_log) { fprintf(stderr, "cannot write %s\n", g_logpath); return; }

	fprintf(g_log, "# sticktest\n");
	fprintf(g_log, "# device      %s\n", name ? name : "?");
	fprintf(g_log, "# sdl mapping %s\n", is_ctrl ? "yes" : "no");
	fprintf(g_log, "# hidapi hint %s\n", SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI)
	                                     ? SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI) : "default");
	fprintf(g_log, "# axes %d, buttons %d, hats %d\n", g_naxes, nbtn, nhat);
	fprintf(g_log, "# raw int16 values, one row per CHANGE (not per poll)\n");
	fprintf(g_log, "ms");
	for (int i = 0; i < g_naxes; i++) fprintf(g_log, ",a%d", i);
	fprintf(g_log, "\n");
	fflush(g_log);
}

static void log_summary(int nsticks, Uint32 ms, long polls, long rows)
{
	if (!g_log) return;
	fprintf(g_log, "\n# ---------------- summary ----------------\n");
	fprintf(g_log, "# duration %.1f s, %ld polls (%.0f Hz), %ld logged rows\n",
	        ms / 1000.0, polls, polls * 1000.0 / (ms ? ms : 1), rows);

	fprintf(g_log, "#\n# axis  min      max      rest      values  step\n");
	for (int i = 0; i < g_naxes; i++) {
		int step, nd = distinct_and_step(&g_axis[i], &step);
		fprintf(g_log, "# a%-3d %+.4f  %+.4f  %+.5f  %6d  %d\n", i,
		        g_axis[i].min < 1e8 ? g_axis[i].min : 0.0,
		        g_axis[i].max > -1e8 ? g_axis[i].max : 0.0,
		        g_axis[i].rest, nd, step);
	}

	for (int i = 0; i < nsticks; i++) {
		if (!is_real_stick(i)) continue;
		const stick_t *s = &g_stick[i];
		const axis_t *ax = &g_axis[i * 2], *ay = &g_axis[i * 2 + 1];
		int swept = 0, flat = 0;
		double mn = 1e9, mx = 0.0, sum = 0.0;
		for (int b = 0; b < NBINS; b++) if (s->env[b] > mx) mx = s->env[b];
		for (int b = 0; b < NBINS; b++) {
			if (s->env[b] < mx * 0.5) continue;
			swept++; sum += s->env[b];
			if (s->env[b] < mn) mn = s->env[b];
		}
		fprintf(g_log, "#\n# stick %d (a%d/a%d): %d/%d sectors swept past half reach\n",
		        i, i * 2, i * 2 + 1, swept, NBINS);
		if (!swept) continue;

		double mean = sum / swept, sd = 0.0;
		for (int b = 0; b < NBINS; b++)
			if (s->env[b] >= mx * 0.5) sd += (s->env[b] - mean) * (s->env[b] - mean);
		sd = sqrt(sd / swept);
		for (int b = 0; b < NBINS; b++)
			if (s->env[b] >= mx * 0.5 && s->env[b] < mx * 0.85) flat++;

		fprintf(g_log, "#   rail hits: a%d %ld, a%d %ld of %ld samples\n",
		        i * 2, ax->pinned, i * 2 + 1, ay->pinned, ax->nsamples);

		double xl = fabs(ax->min), xr = fabs(ax->max);
		double yl = fabs(ay->min), yr = fabs(ay->max);
		fprintf(g_log, "#   roundness %.4f (min %.4f max %.4f mean %.4f sd %.4f, %d low)\n",
		        mx > 0 ? mn / mx : 0.0, mn, mx, mean, sd, flat);
		fprintf(g_log, "#   reach X %+.4f / %+.4f (%.2f%%)   Y %+.4f / %+.4f (%.2f%%)\n",
		        ax->min, ax->max,
		        (xl > 0 && xr > 0) ? fmin(xl, xr) / fmax(xl, xr) * 100.0 : 0.0,
		        ay->min, ay->max,
		        (yl > 0 && yr > 0) ? fmin(yl, yr) / fmax(yl, yr) * 100.0 : 0.0);
		fprintf(g_log, "#   drift %.5f (rest x %+.5f y %+.5f)\n",
		        hypot(ax->rest, ay->rest), ax->rest, ay->rest);
		fprintf(g_log, "#   envelope: sector-centre deg, max radius, visits\n");
		for (int b = 0; b < NBINS; b++)
			fprintf(g_log, "#   env %d %6.1f %.4f %ld\n", i,
			        (b + 0.5) * 360.0 / NBINS, s->env[b], s->visits[b]);
	}
	fflush(g_log);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	signal(SIGINT, on_sigint);
	const char *logpath = (argc > 1) ? argv[1] : NULL;

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}

	printf("waiting for a controller — press a button on it (Ctrl-C to quit)\n");
	SDL_Joystick *js = NULL;
	while (!js && !s_quit) {
		SDL_PumpEvents();
		SDL_JoystickUpdate();
		if (SDL_NumJoysticks() > 0) js = SDL_JoystickOpen(0);
		if (!js) SDL_Delay(100);
	}
	if (!js) { SDL_Quit(); return 0; }

	const char *name = SDL_JoystickName(js);
	int is_ctrl = SDL_IsGameController(0);
	g_naxes = SDL_JoystickNumAxes(js);
	if (g_naxes > MAX_AXES) g_naxes = MAX_AXES;
	int nbtn = SDL_JoystickNumButtons(js), nhat = SDL_JoystickNumHats(js);
	int nsticks = g_naxes / 2;

	axis_reset();
	log_open(argv[0], logpath, name, nbtn, nhat, is_ctrl);
	fputs("\033[2J\033[?25l", stdout);

	Uint32 last_draw = 0, t0 = SDL_GetTicks();
	long polls = 0, rows = 0;
	Sint16 raw[MAX_AXES], prev_raw[MAX_AXES];
	int have_prev = 0;

	while (!s_quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) s_quit = 1;
		SDL_JoystickUpdate();

		for (int i = 0; i < g_naxes; i++) {
			raw[i] = SDL_JoystickGetAxis(js, i);
			double v = raw[i] / 32767.0;
			if (v < -1.0) v = -1.0;
			axis_t *a = &g_axis[i];
			a->now = v;
			if (v < a->min) a->min = v;
			if (v > a->max) a->max = v;
			a->seen[(raw[i] + 32768) >> 3] |= 1u << ((raw[i] + 32768) & 7);
			if (fabs(v) < 0.25) {
				a->rest = a->rest_init ? a->rest * 0.995 + v * 0.005 : v;
				a->rest_init = 1;
			}
			if (raw[i] <= -32700 || raw[i] >= 32700) a->pinned++;
			a->nsamples++;
		}

		// one row per change, not per poll — a still stick would otherwise write
		// ~1000 identical rows a second and bury the movement.
		if (g_log && (!have_prev || memcmp(raw, prev_raw, g_naxes * sizeof(Sint16)) != 0)) {
			fprintf(g_log, "%u", (unsigned)(SDL_GetTicks() - t0));
			for (int i = 0; i < g_naxes; i++) fprintf(g_log, ",%d", raw[i]);
			fputc('\n', g_log);
			memcpy(prev_raw, raw, g_naxes * sizeof(Sint16));
			have_prev = 1;
			if ((++rows & 255) == 0) fflush(g_log);
		}

		for (int i = 0; i < nsticks; i++) {
			stick_t *s = &g_stick[i];
			double x = g_axis[i * 2].now, y = g_axis[i * 2 + 1].now;

			// The pad reports on change, not on a clock, so a brisk sweep can step
			// several degrees between samples and skip a sector entirely. Fill along
			// the segment from the previous position, or the envelope reads as full
			// of notches that are really sampling gaps.
			if (s->has_prev && hypot(s->px, s->py) > 0.15 && hypot(x, y) > 0.15) {
				for (int k = 1; k <= 32; k++) {
					double t = k / 32.0;
					env_hit(s, s->px + (x - s->px) * t, s->py + (y - s->py) * t);
				}
			} else {
				env_hit(s, x, y);
			}
			s->px = x; s->py = y; s->has_prev = 1;
		}
		polls++;

		Uint32 now = SDL_GetTicks();
		if (now - last_draw < 40) { SDL_Delay(1); continue; }
		last_draw = now;

		fputs("\033[H", stdout);
		printf("\033[1mAnalog Stick Tester\033[0m  \033[90m%s%s\033[0m\033[K\n",
		       name ? name : "?", is_ctrl ? "  [has SDL mapping]" : "  [no SDL mapping]");
		printf("\033[90m%d axes, %d buttons, %d hats   %ld polls   %.0f Hz\033[0m\033[K\n\033[K\n",
		       g_naxes, nbtn, nhat, polls,
		       polls * 1000.0 / (double)(now - t0 > 0 ? now - t0 : 1));

		for (int i = 0; i < g_naxes; i++) {
			axis_t *a = &g_axis[i];
			int step, nd = distinct_and_step(a, &step);
			printf(" a%-2d %+.4f ", i, a->now);
			bar(a->now);
			printf(" \033[90mmin %+.3f  max %+.3f  rest %+.4f  %5d values, step %d\033[0m\033[K\n",
			       a->min < 1e8 ? a->min : 0.0, a->max > -1e8 ? a->max : 0.0, a->rest, nd, step);
		}
		fputs("\033[K\n", stdout);

		int sl[MAX_AXES / 2], nsl = 0;
		for (int i = 0; i < nsticks; i++) if (is_real_stick(i)) sl[nsl++] = i;

		// two sticks side by side
		for (int q = 0; q < nsl; q += 2) {
			int pair = sl[q], have2 = (q + 1 < nsl), pair1 = have2 ? sl[q + 1] : 0;
			char g0[GH][GW], g1[GH][GW];
			grid_build(g0, &g_stick[pair], g_axis[pair * 2].now, g_axis[pair * 2 + 1].now);
			if (have2)
				grid_build(g1, &g_stick[pair1], g_axis[pair1 * 2].now,
				           g_axis[pair1 * 2 + 1].now);

			printf("  \033[90mstick %d (a%d/a%d)\033[0m", pair, pair * 2, pair * 2 + 1);
			if (have2) {
				for (int p = 0; p < GW - 15; p++) putchar(' ');
				printf("  \033[90mstick %d (a%d/a%d)\033[0m", pair1, pair1 * 2, pair1 * 2 + 1);
			}
			fputs("\033[K\n", stdout);

			for (int r = 0; r < GH; r++) {
				fputs("  ", stdout);
				grid_print_row(g0, r);
				if (have2) { fputs("    ", stdout); grid_print_row(g1, r); }
				fputs("\033[K\n", stdout);
			}
			fputs("\033[K\n", stdout);
		}

		for (int i = 0; i < nsticks; i++) if (is_real_stick(i)) report_stick(i);

		fputs("\033[K\n \033[90mbuttons:\033[0m ", stdout);
		for (int i = 0; i < nbtn; i++) {
			if (SDL_JoystickGetButton(js, i)) printf("\033[97;1m%d\033[0m ", i);
			else printf("\033[90m%d\033[0m ", i);
		}
		printf("\033[K\n\033[K\n \033[90mlogging %ld rows to %s — Ctrl-C to quit\033[0m\033[K\033[J\n",
		       rows, g_logpath);
		fflush(stdout);
	}

	log_summary(nsticks, SDL_GetTicks() - t0, polls, rows);
	if (g_log) fclose(g_log);
	fputs("\033[?25h\n", stdout);
	printf("wrote %s (%ld rows)\n", g_logpath, rows);
	SDL_JoystickClose(js);
	SDL_Quit();
	return 0;
}
