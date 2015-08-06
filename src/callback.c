#define _GNU_SOURCE
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <stdbool.h>
#include <ffi.h>
#include <inttypes.h>

#include "builtins.h"
#include "variables.h"
#include "arrayfunc.h"
#include "common.h"
#include "bashgetopt.h"
#include "make_cmd.h"
#include "execute_cmd.h"
#include "util.h"
#include "types.h"
#include "shell.h"

// This function gains control when native code calls a callback we generated.
// The ffi_cif and parameters are already setup, we just need to decode them and
// pass them as prefixed types to the bash function.
//
//  retval is where to store the return code that native code will see.
//  args is the argument list native code is trying to pass.
//  uarg is the name of the bash function being called, and the parameter formats.
static void execute_bash_trampoline(ffi_cif *cif, void *retval, void **args, void *uarg)
{
    SHELL_VAR *function;
    WORD_LIST *params;
    char *result;
    char **proto = uarg;
    int i;

    // The first entry in proto is the name of the bash function.
    if (!(function = find_function(*proto))) {
        fprintf(stderr, "error: unable to resolve function %s during callback", *proto);
        return;
    }

    // The remaining entries in proto are the bash prefix formats. The list
    // must be made in reverse order.
    for (params = NULL, i = cif->nargs - 1; i >= 0; i--) {
        char *parameter;

        // Decode the parameters
        parameter = encode_primitive_type(proto[i+1], cif->arg_types[i], args[i]);

        // Add to argument list
        params = make_word_list(make_word(parameter), params);

        // Free our copy
        free(parameter);
    }

    // The first parameter should be the return location
    asprintf(&result, "pointer:%p", retval);

    params = make_word_list(make_word(result), params);
    params = make_word_list(make_word(*proto), params);

    execute_shell_function(function, params);

    free(result);
    return;
}

static int generate_native_callback(WORD_LIST *list)
{
    int nargs;
    void *callback;
    ffi_cif *cif;
    ffi_closure *closure;
    ffi_type **argtypes;
    ffi_type *rettype;
    ffi_type *callbacktype;
    char **proto;
    char *resultname = "DLRETVAL";
    char opt;
    reset_internal_getopt();

    while ((opt = internal_getopt(list, "d:n:")) != -1) {
        switch (opt) {
            case 'n':
                resultname = list_optarg;
                break;
            case 'd':
                // Callbacks are stored as pointers.
                callbacktype = &ffi_type_pointer;

                // Attempt to decode the specified callback.
                if (decode_primitive_type(list_optarg, &callback, &callbacktype) != true) {
                    builtin_error("failed to decode callback from parameter %s", list_optarg);
                    return EXECUTION_FAILURE;
                }

                // FIXME convert executable address to a writable address.

                // And free the value generated by decode_primitive_type
                free(callback);

                return 0;
            default:
                builtin_usage();
                return EX_USAGE;
        }
    }

    // Skip past any options.
    if ((list = loptend) == NULL || !list->next) {
        builtin_usage();
        return EX_USAGE;
    }

    closure     = ffi_closure_alloc(sizeof(ffi_closure), &callback);
    cif         = malloc(sizeof(ffi_cif));
    argtypes    = NULL;
    proto       = malloc(sizeof(char *));
    proto[0]    = strdup(list->word->word);
    nargs       = 0;
    list        = list->next;

    // Second parameter must be the return type
    if (decode_type_prefix(list->word->word,
                           NULL,
                           &rettype,
                           NULL,
                           NULL) != true) {
        builtin_warning("couldnt parse the return type %s", list->word->word);
        return EXECUTION_FAILURE;
    }

    // Skip past return type
    list = list->next;

    while (list) {
        argtypes        = realloc(argtypes, (nargs + 1) * sizeof(ffi_type *));
        proto           = realloc(proto, (nargs + 1 + 1) * sizeof(char *));

        if (decode_type_prefix(list->word->word, NULL, &argtypes[nargs], NULL, &proto[nargs+1]) != true) {
            builtin_error("failed to decode type from parameter %s", list->word->word);
            goto error;
        }

        list = list->next;
        nargs++;
    }

    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, nargs, rettype, argtypes) == FFI_OK) {
        // Initialize the closure.
        if (ffi_prep_closure_loc(closure, cif, execute_bash_trampoline, proto, callback) == FFI_OK) {
            char retval[1024];
            snprintf(retval, sizeof retval, "pointer:%p", callback);

            // Output if this shell is interactive.
            if (interactive_shell) {
                fprintf(stderr, "%s\n", retval);
            }

            bind_variable(resultname, retval, 0);
        }
    }

    //free(argtypes);
    return 0;

  error:
    //free(argtypes);
    return 1;
}


static char *callback_usage[] = {
    "callback function returntype [parametertype...]",
    "Generate a native callable function pointer",
    "",
    "It is sometimes necessary to provide a callback function to library",
    "routines, for example bsearch and qsort. Given a bash function name and a",
    "list of type prefixes, this routine will return a function pointer that",
    "can be called from native code.",
    "",
    "functions in bash can only return small integers <= 255, so ctypes.sh",
    "uses pointers to pass return values. The first parameter to your callback",
    "is a pointer to the location to write your return value (if required).",
    "If you need to directly write to the return value, use the pack command.",
    "",
    "",
    "Options:",
    "    -n name      Store the callback generated in name, not DLRETVAL.",
    "    -d callback  Free previously allocated callback",
    "",
    "Usage:",
    "",
    " $ function bash_callback() {",
    " > echo hello from bash",
    " > return 1",
    " > }",
    " $ callback bash_callback int int int",
    " pointer:0x123123",
    "",
    NULL,
};

struct builtin __attribute__((visibility("default"))) callback_struct = {
    .name       = "callback",
    .function   = generate_native_callback,
    .flags      = BUILTIN_ENABLED,
    .long_doc   = callback_usage,
    .short_doc  = "callback [-n name] [-d callback] function returntype [parametertype] [...]",
    .handle     = NULL,
};

