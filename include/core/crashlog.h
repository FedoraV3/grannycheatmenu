#ifndef CRASHLOG_H
#define CRASHLOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install a fault logger.
 *
 * Writes the faulting address of any access violation to
 * %APPDATA%\grannycheat\crash.log, resolved to a module and an RVA, along
 * with which features were switched on at the time.
 *
 * This exists because a crash that cannot be reproduced on the machine doing
 * the reading is otherwise diagnosed by guesswork, and guessing has already
 * cost two wrong fixes. An RVA inside GameAssembly.dll can be looked up
 * directly in IDA; one inside libcheat.dll points at our own code.
 *
 * Uses a vectored handler so it runs before Unity's own crash handling. It
 * only records genuinely fatal exception codes -- IL2CPP does its null
 * checks explicitly rather than by faulting, so an access violation here is
 * a real one and not ordinary managed-exception traffic.
 *
 * Call once, early. Safe to call more than once; only the first takes.
 */
void crashlog_init(void);

/**
 * @brief Leave a breadcrumb for the log.
 *
 * The last few are written out alongside the fault, which turns "it crashed
 * somewhere" into "it crashed just after this". Cheap enough to call from a
 * per-frame hook: it copies a pointer, not a string.
 *
 * @param what A STATIC string -- it is not copied, so it must outlive the
 *             call. String literals are the intended use.
 */
void crashlog_mark(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* CRASHLOG_H */
