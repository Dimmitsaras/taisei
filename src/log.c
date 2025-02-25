/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include <SDL_bits.h>
#include <SDL_mutex.h>

#include "log.h"
#include "util.h"
#include "list.h"
#include "util/strbuf.h"

typedef struct Logger {
	LIST_INTERFACE(struct Logger);

	FormatterObj formatter;
	SDL_RWops *out;
	uint levels;
} Logger;

typedef struct QueuedLogEntry {
	LIST_INTERFACE(struct QueuedLogEntry);
	LogEntry e;
	char message[];
} QueuedLogEntry;

static struct {
	Logger *outputs;
	size_t num_outputs;
	SDL_mutex *mutex;
	uint enabled_log_levels;

	struct {
		StringBuffer pre_format;
		StringBuffer format;
	} buffers;

	struct {
		SDL_Thread *thread;
		SDL_mutex *mutex;
		SDL_cond *cond;
		LIST_ANCHOR(QueuedLogEntry) queue;
		int shutdown;
	} queue;

	bool initialized;

#ifdef LOG_FATAL_MSGBOX
	char *err_appendix;
#endif
} logging;

#define INIT_BUF_SIZE 0x100
#define MAX_BUF_SIZE 0x10000

static const char *level_prefix_map[] = {
	[_LOG_DEBUG_ID] = "D",
	[_LOG_INFO_ID]  = "I",
	[_LOG_WARN_ID]  = "W",
	[_LOG_ERROR_ID] = "E",
	[_LOG_FATAL_ID] = "F",
};

static const char *level_name_map[] = {
	[_LOG_DEBUG_ID] = "DEBUG",
	[_LOG_INFO_ID]  = "INFO",
	[_LOG_WARN_ID]  = "WARNING",
	[_LOG_ERROR_ID] = "ERROR",
	[_LOG_FATAL_ID] = "FATAL",
};

attr_unused
static const char *level_ansi_style_map[] = {
	[_LOG_DEBUG_ID] = "\x1b[1;35m",
	[_LOG_INFO_ID]  = "\x1b[1;32m",
	[_LOG_WARN_ID]  = "\x1b[1;33m",
	[_LOG_ERROR_ID] = "\x1b[1;31m",
	[_LOG_FATAL_ID] = "\x1b[1;31m",
};

#define INDEX_MAP(map, lvl) \
	uint idx = SDL_MostSignificantBitIndex32(lvl); \
	assert_nolog(idx < sizeof(map) / sizeof(*map)); \
	return map[idx];

static const char* level_prefix(LogLevel lvl) { INDEX_MAP(level_prefix_map, lvl) }
static const char* level_name(LogLevel lvl) { INDEX_MAP(level_name_map, lvl) }
attr_unused static const char* level_ansi_style_code(LogLevel lvl) { INDEX_MAP(level_ansi_style_map, lvl) }

static void log_queue_shutdown(bool force_sync);

noreturn static void log_abort(const char *msg) {
#ifdef LOG_FATAL_MSGBOX
	static const char *const title = "Taisei: fatal error";

	if(msg) {
		if(logging.err_appendix) {
			char *m = strfmt("%s\n\n%s", msg, logging.err_appendix);
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, m, NULL);
			free(m);
		} else {
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
		}
	} else if(logging.err_appendix) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, logging.err_appendix, NULL);
	}
#endif

	log_queue_shutdown(true);
	log_shutdown();

	// abort() doesn't clean up, but it lets us get a backtrace, which is more useful
	abort();
}

void log_set_gui_error_appendix(const char *message) {
#ifdef LOG_FATAL_MSGBOX
	stralloc(&logging.err_appendix, message);
#endif
}

static void add_debug_info(StringBuffer *buf) {
	IF_DEBUG({
		DebugInfo *debug_info = get_debug_info();
		DebugInfo *debug_meta = get_debug_meta();

		strbuf_printf(buf,
			"\n\n\n"
			"Debug info: %s:%i:%s\n"
			"Debug info set at: %s:%i:%s\n"
			"Note: debug info may not be relevant to this issue\n",
			debug_info->file, debug_info->line, debug_info->func,
			debug_meta->file, debug_meta->line, debug_meta->func
		);
	});
}

static void log_dispatch(LogEntry *entry) {
	StringBuffer *fmt_buf = &logging.buffers.format;

	for(Logger *l = logging.outputs; l; l = l->next) {
		if(l->levels & entry->level) {
			strbuf_clear(fmt_buf);
			size_t slen = l->formatter.format(&l->formatter, fmt_buf, entry);
			assert_nolog(fmt_buf->buf_size >= slen);
			SDL_RWwrite(l->out, fmt_buf->start, 1, slen);
		}
	}
}

static void log_dispatch_async(LogEntry *entry) {
	for(Logger *l = logging.outputs; l; l = l->next) {
		if(l->levels & entry->level) {
			size_t msg_len = strlen(entry->message);
			QueuedLogEntry *qle = calloc(1, sizeof(*qle) + msg_len + 1);
			memcpy(&qle->e, entry, sizeof(*entry));
			memcpy(qle->message, entry->message, msg_len);
			qle->e.message = qle->message;
			SDL_LockMutex(logging.queue.mutex);
			alist_append(&logging.queue.queue, qle);
			SDL_CondBroadcast(logging.queue.cond);
			SDL_UnlockMutex(logging.queue.mutex);
			break;
		}
	}
}

static void *sync_logger(List **loggers, List *logger, void *arg) {
	Logger *l = (Logger*)logger;
	SDL_RWsync(l->out);
	return NULL;
}

static void log_internal(LogLevel lvl, const char *funcname, const char *filename, uint line, const char *fmt, va_list args) {
	assert(fmt[strlen(fmt)-1] != '\n');

	bool noabort = lvl & LOG_NOABORT_BIT;
	lvl &= logging.enabled_log_levels;

	if(lvl == LOG_NONE) {
		return;
	}

	SDL_LockMutex(logging.mutex);

	StringBuffer *buf = &logging.buffers.pre_format;
	strbuf_clear(buf);

	va_list args_copy;
	va_copy(args_copy, args);
	attr_unused int slen = strbuf_vprintf(buf, fmt, args_copy);
	va_end(args_copy);
	assert_nolog(slen >= 0);

	if(lvl & LOG_FATAL) {
		add_debug_info(buf);
	}

	LogEntry entry = {
		.message = buf->start,
		.file = filename,
		.func = funcname,
		.line = line,
		.level = lvl,
		.time = SDL_GetTicks(),
	};

	if(logging.queue.thread) {
		log_dispatch_async(&entry);
	} else {
		log_dispatch(&entry);
	}

	if(lvl & LOG_FATAL) {
		if(noabort) {
			// Will likely abort externally (e.g. assertion failure), so sync everything now.
			list_foreach(&logging.outputs, sync_logger, NULL);
		} else {
			log_abort(entry.message);
		}
	}

	SDL_UnlockMutex(logging.mutex);
}

void _taisei_log(LogLevel lvl, const char *funcname, const char *filename, uint line, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_internal(lvl, funcname, filename, line, fmt, args);
	va_end(args);
}

void _taisei_log_fatal(LogLevel lvl, const char *funcname, const char *filename, uint line, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_internal(lvl, funcname, filename, line, fmt, args);
	va_end(args);

	// should usually not get here, log_internal will abort earlier if lvl is LOG_FATAL
	// that is unless LOG_FATAL is disabled for some reason
	log_abort(NULL);
}

static void *delete_logger(List **loggers, List *logger, void *arg) {
	Logger *l = (Logger*)logger;

	if(l->formatter.free != NULL) {
		l->formatter.free(&l->formatter);
	}

	SDL_RWsync(l->out);
	SDL_RWclose(l->out);
	free(list_unlink(loggers, logger));

	return NULL;
}

static int log_queue_thread(void *a) {
	SDL_mutex *mtx = logging.queue.mutex;
	SDL_cond *cond = logging.queue.cond;

	SDL_LockMutex(mtx);

	for(;;) {
		SDL_CondWait(cond, mtx);
		QueuedLogEntry *qle;

		while((qle = alist_pop(&logging.queue.queue)) && logging.queue.shutdown < 2) {
			SDL_UnlockMutex(mtx);
			log_dispatch(&qle->e);
			free(qle);
			SDL_LockMutex(mtx);
		}

		SDL_CondBroadcast(cond);

		if(logging.queue.shutdown) {
			break;
		}
	}

	SDL_UnlockMutex(mtx);
	return 0;
}

static void log_queue_init(void) {
	if(!env_get("TAISEI_LOG_ASYNC", true)) {
		return;
	}

	if(!(logging.queue.cond = SDL_CreateCond())) {
		log_sdl_error(LOG_ERROR, "SDL_CreateCond");
		return;
	}

	if(!(logging.queue.mutex = SDL_CreateMutex())) {
		log_sdl_error(LOG_ERROR, "SDL_CreateMutex");
		return;
	}

	if(!(logging.queue.thread = SDL_CreateThread(log_queue_thread, "Log queue", NULL))) {
		log_sdl_error(LOG_ERROR, "SDL_CreateThread");
		return;
	}
}

static void log_queue_shutdown(bool force_sync) {
	if(!logging.queue.thread) {
		return;
	}

	SDL_LockMutex(logging.queue.mutex);
	logging.queue.shutdown = env_get("TAISEI_LOG_ASYNC_FAST_SHUTDOWN", false) && !force_sync ? 2 : 1;
	SDL_CondBroadcast(logging.queue.cond);
	SDL_UnlockMutex(logging.queue.mutex);
	SDL_WaitThread(logging.queue.thread, NULL);
	logging.queue.thread = NULL;
	SDL_DestroyMutex(logging.queue.mutex);
	logging.queue.mutex = NULL;
	SDL_DestroyCond(logging.queue.cond);
	logging.queue.cond = NULL;
}

void log_init(LogLevel lvls) {
	logging.enabled_log_levels = lvls;
	logging.mutex = SDL_CreateMutex();
	log_queue_init();
	logging.initialized = true;
}

void log_shutdown(void) {
	logging.initialized = false;
	log_queue_shutdown(false);
	list_foreach(&logging.outputs, delete_logger, NULL);
	SDL_DestroyMutex(logging.mutex);
	strbuf_free(&logging.buffers.pre_format);
	strbuf_free(&logging.buffers.format);

#ifdef LOG_FATAL_MSGBOX
	free(logging.err_appendix);
#endif

	memset(&logging, 0, sizeof(logging));
}

void log_sync(void) {
	SDL_LockMutex(logging.queue.mutex);
 	while(logging.queue.queue.first) {
		SDL_CondWait(logging.queue.cond, logging.queue.mutex);
	}
	SDL_UnlockMutex(logging.queue.mutex);
}

bool log_initialized(void) {
	return logging.initialized;
}

void log_add_output(LogLevel levels, SDL_RWops *output, Formatter *formatter) {
	if(!output) {
		return;
	}

	if(!(levels & logging.enabled_log_levels)) {
		SDL_RWclose(output);
		return;
	}

	Logger *l = calloc(1, sizeof(Logger));
	l->levels = levels;
	l->out = output;

	formatter(&l->formatter, l->out);
	assert(l->formatter.format != NULL);

	list_append(&logging.outputs, l);
}

static LogLevel chr2lvl(char c) {
	c = toupper(c);

	for(int i = 0; i < sizeof(level_prefix_map) / sizeof(char*); ++i) {
		if(c == level_prefix_map[i][0]) {
			return (1 << i);
		} else if(c == 'A') {
			return LOG_ALL;
		}
	}

	return 0;
}

LogLevel log_parse_levels(LogLevel lvls, const char *lvlmod) {
	if(!lvlmod) {
		return lvls;
	}

	bool enable = true;

	for(const char *c = lvlmod; *c; ++c) {
		if(*c == '+') {
			enable = true;
		} else if(*c == '-') {
			enable = false;
		} else if(enable) {
			lvls |= chr2lvl(*c);
		} else {
			lvls &= ~chr2lvl(*c);
		}
	}

	return lvls;
}

static int log_fmtconsole_format_ansi(FormatterObj *obj, StringBuffer *buf, LogEntry *entry) {
	return strbuf_printf(
		buf,
		"\x1b[1;30m%-9d %s%s\x1b[1;30m: \x1b[1;34m%s\x1b[1;30m: \x1b[0m%s\n",
		entry->time,
		level_ansi_style_code(entry->level),
		level_prefix(entry->level),
		entry->func,
		entry->message
	);
}

static int log_fmtconsole_format_plain(FormatterObj *obj, StringBuffer *buf, LogEntry *entry) {
	return strbuf_printf(
		buf,
		"%-9d %s: %s: %s\n",
		entry->time,
		level_prefix(entry->level),
		entry->func,
		entry->message
	);
}

#ifdef TAISEI_BUILDCONF_HAVE_POSIX
#include <unistd.h>

static bool output_supports_ansi_sequences(const SDL_RWops *output) {
	if(!strcmp(env_get("TERM", "dumb"), "dumb")) {
		return false;
	}

	if(output->type != SDL_RWOPS_STDFILE) {
		return false;
	}

	return isatty(fileno(output->hidden.stdio.fp));
}
#else
static bool output_supports_ansi_sequences(const SDL_RWops *output) {
	// TODO: Handle it for windows. The windows console supports ANSI escapes, but requires special setup.
	return false;
}
#endif

void log_formatter_console(FormatterObj *obj, const SDL_RWops *output) {
	if(output_supports_ansi_sequences(output)) {
		obj->format = log_fmtconsole_format_ansi;
	} else {
		obj->format = log_fmtconsole_format_plain;
	}
}

static int log_fmtfile_format(FormatterObj *obj, StringBuffer *buf, LogEntry *entry) {
	return strbuf_printf(
		buf,
		"%d  %s  %s: %s\n",
		entry->time,
		level_name(entry->level),
		entry->func,
		entry->message
	);
}

void log_formatter_file(FormatterObj *obj, const SDL_RWops *output) {
	obj->format = log_fmtfile_format;
}
