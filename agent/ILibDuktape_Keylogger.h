#ifndef ILIB_DUKTAPE_KEYLOGGER_H
#define ILIB_DUKTAPE_KEYLOGGER_H
#include "duktape.h"
void ILibDuktape_Keylogger_Init(duk_context *ctx);
#ifdef WIN32
/* Entry point for the user-session keylog worker process
 * (launched as: MeshAgent.exe keylog-worker [windowTitles]). */
int ILibDuktape_KeyloggerWorker_Run(int argc, char **argv);
#endif
#endif
