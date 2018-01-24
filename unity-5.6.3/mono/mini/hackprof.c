
#include <stdio.h>
#include <time.h>

#include "mini.h"
#include "hackprof.h"

/*
 Map MonoThread -> HackprofTlsData.
 TODO: Find out if and how to properly lock access to this, because various threads can write to it
 (only the profile agent will read from it, though)
*/
static GHashTable *thread_to_tls;

/* Size of the profiling data buffer (FIXME: Hide somewhere) */
#define SBUF_SIZE 32768

/* Hackprof thread-specific data (for all runtime threads)´*/
typedef struct {
	/* normally 0, set to something else when a command is available in command_buffer */
	int command_pending;
	/* a command that is to be executed, debugger thread puts it here */
	int command_buffer;

	/* Whether this thread is currently doing profile actions.
	Only set by the thread itself in response to a command (so that it can perform cleanup work, close files etc.)
	*/
	int is_profiled;

	FILE *profile_fh;
	FILE *profile_bin_fh;
	GHashTable *method_info;
	char sbuf[SBUF_SIZE];
	int sbuf_usage;
} HackprofTlsData;

static guint32 hackprof_tls_id;

/* Allocate and initialize threadhackprof thread-specific data. Called in the same thread that "thread" refers to.
 */
void hackprof_init_new_thread(MonoThread *thread) {
	HackprofTlsData *htls = g_new0(HackprofTlsData, 1);

	htls->command_pending = 0;

	/* FIXME: Allow threads to be profiled from start? Query somewhere else if profiling should happen */
	htls->is_profiled = 0;
	htls->sbuf_usage = 0;

	/* Open profile log file */
	time_t t;
	struct tm *tmp;
	t = time(NULL);
	tmp = localtime(&t);

	char thefilename[256];
	char datepart[20];
	strftime(datepart, sizeof(datepart), "%Y%m%d-%H%M%S", tmp);
	snprintf(thefilename, 256, "callgrind.%s.%p.out", datepart, (gpointer)thread->tid);
	htls->profile_fh = fopen(thefilename, "w"); /* FIXME handle error */

	/* Also open a binary log file */
	char binfilename[256];
	snprintf(binfilename, 256, "callgrind.%s.%p.bin", datepart, (gpointer)thread->tid);
	htls->profile_bin_fh = fopen(binfilename, "wb"); /* FIXME handle error */

	/* Known method information */
	htls->method_info = g_hash_table_new(mono_aligned_addr_hash, NULL); /* FIXME can we give this table some initial size? */

	/* More info dump */
	fprintf(htls->profile_fh, "HELLO WORLD\n");
	LARGE_INTEGER cFreq;
	QueryPerformanceFrequency(&cFreq);
	fprintf(htls->profile_fh, "QPC frequency is %lld t/second\n", cFreq.QuadPart);
	fprintf(htls->profile_fh, "sizeof(long long) = %zd, sizeof(MonoMethod *) = %zd\n", sizeof(cFreq.QuadPart), sizeof(MonoMethod *));
	fflush(htls->profile_fh);

	TlsSetValue(hackprof_tls_id, htls);
	/* added to thread_to_tls in _locked */
}

void hackprof_init_new_thread_locked(MonoThread *thread) {
	g_hash_table_insert(thread_to_tls, thread, TlsGetValue(hackprof_tls_id));
}

static int check_blacklisted(MonoMethod *method) {
	char *typename = mono_type_full_name(&method->klass->byval_arg);
	
	/* The nice thing is that these checks are only run once per method, so they can be quite complex! */
	if (
		/* This gets called too often as part of the game loop (presumably) */
		!strcmp(typename, "System.DateTime")
		|| !strcmp(typename, "System.TimeSpan")
		|| !strcmp(typename, "System.Globalization.DaylightTime")
		|| !strcmp(typename, "System.CurrentSystemTimeZone")
		|| !strcmp(typename, "System.TimeZone")
		/* We don't want to profile UnityEngine, and for mod development we won't get this low usually */
		|| !strcmp(typename, "UnityEngine.Object")
		|| !strcmp(typename, "UnityEngine.Vector2")
		|| !strcmp(typename, "UnityEngine.Vector3")
		|| !strcmp(typename, "UnityEngine.Component")
		|| !strcmp(typename, "UnityEngine.Transform")
		|| !strcmp(typename, "UnityEngine.GameObject")
		|| !strcmp(typename, "UnityEngine.Mathf")
		|| !strcmp(typename, "UnityEngine.Matrix4x4")
		|| !strcmp(typename, "UnityEngine.Behaviour")
		|| !strcmp(typename, "UnityEngine.RectOffset")
		|| !strcmp(typename, "UnityEngine.Font") // Maybe relevant if tracing can be limited to ingame
		// || !strncmp(typename, "UnityEngine.", 12) // tabula rasa
		/* Probably from Unity as well */
		|| !strcmp(typename, "System.Type")
		|| !strcmp(typename, "System.MonoType")
		|| !strcmp(typename, "System.MonoCustomAttrs")
		|| !strcmp(typename, "System.RuntimeTypeHandle")
		|| !strcmp(typename, "System.Reflection.MonoField")
		|| !strcmp(typename, "System.Reflection.FieldInfo")
		|| !strcmp(typename, "System.Reflection.Binder")
		|| !strcmp(typename, "System.Delegate")
		|| !strcmp(typename, "System.MulticastDelegate")

		/* Heavily used data structures */
		|| !strcmp(typename, "System.Array")
		|| !strncmp(typename, "System.Array/", 13)
		|| !strcmp(typename, "System.Collections.Generic.List`1")
		|| !strcmp(typename, "System.Collections.Hashtable")
		|| !strcmp(typename, "System.Collections.Hashtable/Enumerator")
		/* IO, typically heavily fragmented into few-byte reads */
		|| !strcmp(typename, "System.IO.FileStream")
		|| !strcmp(typename, "System.IO.BinaryReader")
		|| !strcmp(typename, "LoadingScreenMod.MemStream")
		|| !strcmp(typename, "LoadingScreenMod.MemReader")
		|| !strncmp(typename, "ColossalFramework.IO.EncodedArray/", 34)
		|| !strcmp(typename, "ColossalFramework.IO.DataSerializer")
		/* Simply not relevant resp. likely to be very cheap calls that just spam logs */
		|| !strcmp(typename, "ColossalFramework.Singleton`1")
		|| !strcmp(typename, "TreeUnlimiter.LimitTreeManager/Helper") // at least the property accesses

		|| !strncmp(typename, "Mono.Globalization.Unicode.", 27)
		|| !strncmp(typename, "ColossalFramework.PoolList`1", 28)
		|| !strncmp(typename, "ColossalFramework.UI.", 21)
		|| !strcmp(typename, "ColossalFramework.ReflectionExtensions")
		|| !strcmp(typename, "ColossalFramework.Packaging.PackageReader")
		|| !strncmp(typename, "System.Linq.Enumerable/", 23)
		|| !strcmp(typename, "System.Collections.Generic.KeyValuePair`2")
		|| !strcmp(typename, "System.Collections.Generic.EqualityComparer`1/DefaultComparer")
		|| !strcmp(typename, "System.Collections.Generic.GenericEqualityComparer`1")
		|| !strcmp(typename, "System.NumberFormatter")
		|| !strncmp(typename, "System.Globalization.", 21)
		|| !strcmp(typename, "System.Text.StringBuilder")
		|| !strcmp(typename, "System.Text.UTF8Encoding")
		|| !strcmp(typename, "System.Text.UTF8Encoding/UTF8Decoder")
		|| !strcmp(typename, "System.Math")

		) {
		return 1;
		/* TODO: We could do a two step exclusion. Full ignore and for other function maybe just
		 * imprecise tracking of total time. What for? Functions that get called very often normally,
		 * which are too expensive to fully track. But if just a running counter has to be maintained
		 * then it might be doable
		 */
	}

	return 0;
}

/*
 * This makes sure that a method's information is gathered and written to the
 * profile file as soon as it is first referenced.
 *
 * Reference it with the pointer value afterwards
 */
#define KNOWN_IGNORED 1
static int ensure_method_known(HackprofTlsData *htls, MonoMethod *method) {
	/* for some reason orig_key and value may not be NULL even though docs say otherwise (old glib?) */
	gpointer orig_key, value;
	if (!g_hash_table_lookup_extended(htls->method_info, method, &orig_key, &value)) {
		/* not yet in table */
		char *fullname = mono_method_full_name(method, TRUE);

		/* if not on blacklist, then add to profile log */
		int ty = 0;
		if (!check_blacklisted(method)) {
			fprintf(htls->profile_fh, "method %p is %s\n", method, fullname);
			fflush(htls->profile_fh);
		}
		else {
			fprintf(htls->profile_fh, "ignored method %p is %s\n", method, fullname);
			fflush(htls->profile_fh);
			ty = KNOWN_IGNORED;
		}

		g_hash_table_insert(htls->method_info, method, ty);

		return ty;
	}
	
	return value;
}

/*
Obtain TLS data for current thread.
Before returning, process pending command, if any, and checks if any profiling is supposed to take place.
If yes, then TLS data is returned to caller, otherwise NULL (caller is supposed to check for this and return early)
*/
static HackprofTlsData *commands_and_htls_if_profiling() {
	HackprofTlsData *htls = TlsGetValue(hackprof_tls_id);

	if (htls->command_pending) {
		/* "Process" command */
		guint32 name_len;
		gunichar2 *s = mono_thread_get_name(mono_thread_current(), &name_len);
		char *name;
		if (!s) {
			name = g_malloc(sizeof(gchar));
			name[0] = 0;
		}
		else {
			name = g_utf16_to_utf8(s, name_len, NULL, NULL, NULL);
			g_assert(name);
		}
		fprintf(htls->profile_fh, "*** Profiling command received: %d - I am \"%s\"\n", htls->command_buffer, name);
		fflush(htls->profile_fh);
		// XXX just enable profiling here
		htls->is_profiled = !htls->is_profiled; // cleanup?! XXX
		g_free(name);
		htls->command_pending = 0;
	}

	return htls->is_profiled ? htls : NULL;
}

/* Do not trace the most basic types at all */
// #define is_filtered_basic_type(ty) (((ty) == MONO_TYPE_STRING) || ((ty) == MONO_TYPE_OBJECT) || ((ty) == MONO_TYPE_I) || ((ty) == MONO_TYPE_CHAR) || ((ty) == MONO_TYPE_I4) || ((ty) == MONO_TYPE_U8))
#define is_filtered_basic_type(ty) (!( ((ty) == MONO_TYPE_CLASS) || ((ty) == MONO_TYPE_GENERICINST) ))

static void hackprof_enter(MonoProfiler *prof, MonoMethod *method) {
	HackprofTlsData *htls = commands_and_htls_if_profiling();
	if (!htls) return;

	/* Quick-ignore some very heavy types */
	MonoTypeEnum ty = method->klass->byval_arg.type;
	if (is_filtered_basic_type(ty)) {
		return;
	}

	LARGE_INTEGER eventTime;
	QueryPerformanceCounter(&eventTime);

	/* Now check on the extended ignore list & make sure it is known in the profile log */
	int known_method = ensure_method_known(htls, method);
	if (known_method != KNOWN_IGNORED) {
		if (htls->sbuf_usage >= SBUF_SIZE) {
			/* :( */
			fwrite(htls->sbuf, SBUF_SIZE, 1, htls->profile_bin_fh);
			htls->sbuf_usage = 0;
		}
		char *ctarget = htls->sbuf + htls->sbuf_usage;
		long long *target = (long long *)ctarget;
		target[0] = eventTime.QuadPart;
		target[1] = method;
		htls->sbuf_usage += 16;
	}
}


static void hackprof_leave(MonoProfiler *prof, MonoMethod *method) { /* FIXME DEDUPLICATION */
	HackprofTlsData *htls = commands_and_htls_if_profiling();
	if (!htls) return;

	/* Quick-ignore some very heavy types */
	MonoTypeEnum ty = method->klass->byval_arg.type;
	if (is_filtered_basic_type(ty)) {
		return;
	}

	LARGE_INTEGER eventTime;
	QueryPerformanceCounter(&eventTime);

	/* Now check on the extended ignore list & make sure it is known in the profile log */
	int known_method = ensure_method_known(htls, method);
	if (known_method != KNOWN_IGNORED) {
		if (htls->sbuf_usage >= SBUF_SIZE) {
			/* :( */
			fwrite(htls->sbuf, SBUF_SIZE, 1, htls->profile_bin_fh);
			fflush(htls->profile_fh); // MEH
			htls->sbuf_usage = 0;
		}
		char *ctarget = htls->sbuf + htls->sbuf_usage;
		long long *target = (long long *) ctarget;
		target[0] = eventTime.QuadPart | 0x8000000000000000ULL;
		target[1] = method;
		htls->sbuf_usage += 16;
	}
}

/*
Initialization code, run once from the profiling agent thread.
Set up global tables and install the profile functions in the runtime.
*/
void hackprof_init_after_agent() {
	hackprof_tls_id = TlsAlloc();

	thread_to_tls = g_hash_table_new(mono_aligned_addr_hash, NULL);

	mono_profiler_install_enter_leave(hackprof_enter, hackprof_leave);
}

/*
Send a profile command to a mono thread.
The command will actually get processed the next time any profileable event occurs
(so when *any* method, even ignored one, is entered or left).
*/
void hackprof_poke_thread(MonoThread *thread, int command) {
	HackprofTlsData *htls = g_hash_table_lookup(thread_to_tls, thread);
	if (htls == NULL) {
		/* Thread doesn't exist in our list ... */
		// TODO: Some kind of error feedback
		return;
	}

	/* FIXME: Multithreading hell */
	if (htls->command_pending) {
		/* Just dropping the command (FIXME) , no it doesn't work that way. */
		/* Additionally, this *could* be changed in the meantime (although we,
		 * the debugger thread, should be the only one to fiddle around here.
		 * Everyone knows about these things that "should be ...". */
		return;
	}

	htls->command_buffer = command;
	htls->command_pending = 1;
}