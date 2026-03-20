#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "py/builtin.h"
#include "py/mpstate.h"
#include "py/misc.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/stackctrl.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "genhdr/mpversion.h"
#include "genhdr/amigaversion.h"

#include <proto/dos.h>
#include <proto/exec.h>

_Static_assert(
    offsetof(mp_state_ctx_t, thread.dict_locals) % 4 == 0,
    "Root pointer alignment broken on m68k - reapply ports/amiga/patches/mpstate_alignment.patch"
);

// Tell AmigaOS/libnix to allocate a 128 KB stack for this process.
long __stack = 131072;

// StackSwap support — if the shell didn't provide enough stack, we allocate our own.
#include <exec/tasks.h>
#define MIN_STACK_SIZE 65536

static char *new_stack = NULL;
static struct StackSwapStruct stack_swap;
static int stack_swapped = 0;

static void ensure_stack(void) {
    struct Task *me = FindTask(NULL);
    ULONG current_stack = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;
    if (current_stack < MIN_STACK_SIZE) {
        new_stack = (char *)AllocMem(MIN_STACK_SIZE, MEMF_ANY);
        if (new_stack) {
            stack_swap.stk_Lower = new_stack;
            stack_swap.stk_Upper = (ULONG)(new_stack + MIN_STACK_SIZE);
            stack_swap.stk_Pointer = (APTR)(new_stack + MIN_STACK_SIZE);
            StackSwap(&stack_swap);
            stack_swapped = 1;
        }
    }
}

static void restore_stack(void) {
    if (stack_swapped) {
        StackSwap(&stack_swap);
        FreeMem(new_stack, MIN_STACK_SIZE);
        new_stack = NULL;
        stack_swapped = 0;
    }
}

static char *stack_top;
static char *heap = NULL;
static unsigned long heap_size = MICROPY_HEAP_SIZE;
BPTR original_dir = 0;

// quit() and exit() builtins — raise SystemExit
static mp_obj_t mp_builtin_quit(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0) {
        mp_raise_type(&mp_type_SystemExit);
    } else {
        mp_raise_type_arg(&mp_type_SystemExit, args[0]);
    }
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_builtin_quit_obj, 0, 1, mp_builtin_quit);
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_builtin_exit_obj, 0, 1, mp_builtin_quit);

// Custom REPL using readline with cursor keys and history.
static void do_repl(void) {
    mp_hal_stdout_tx_str("MicroPython v" MICROPY_VERSION_STRING " on " AMIGA_BUILD_TIMESTAMP
        " build " AMIGA_BUILD_NUM "; " MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME "\r\n");
    printf("Heap: %luKB (available: %luKB)\r\n",
           heap_size / 1024, (unsigned long)AvailMem(MEMF_ANY) / 1024);
    mp_hal_stdout_tx_str("Use Ctrl-D to exit, Ctrl-E for paste mode\r\n");
    mp_hal_stdout_tx_str("Type \"help()\" for more information.\r\n");

    mp_hal_stdio_mode_raw();

    vstr_t line;
    vstr_init(&line, 32);

    for (;;) {
        MP_STATE_THREAD(gc_lock_depth) = 0;
        vstr_reset(&line);
        int ret = readline(&line, ">>> ");
        mp_parse_input_kind_t parse_input_kind = MP_PARSE_SINGLE_INPUT;

        if (ret == CHAR_CTRL_C) {
            mp_hal_stdout_tx_str("\r\n");
            continue;
        } else if (ret == CHAR_CTRL_D) {
            mp_hal_stdout_tx_str("\r\n");
            break;
        } else if (ret == CHAR_CTRL_E) {
            // Paste mode
            mp_hal_stdout_tx_str("\r\npaste mode; Ctrl-C to cancel, Ctrl-D to finish\r\n=== ");
            vstr_reset(&line);
            for (;;) {
                char c = mp_hal_stdin_rx_chr();
                if (c == CHAR_CTRL_C) {
                    mp_hal_stdout_tx_str("\r\n");
                    goto input_restart;
                } else if (c == CHAR_CTRL_D) {
                    mp_hal_stdout_tx_str("\r\n");
                    break;
                } else {
                    vstr_add_byte(&line, c);
                    if (c == '\r') {
                        mp_hal_stdout_tx_str("\r\n=== ");
                    } else {
                        mp_hal_stdout_tx_strn(&c, 1);
                    }
                }
            }
            parse_input_kind = MP_PARSE_FILE_INPUT;
        } else if (vstr_len(&line) == 0) {
            continue;
        } else {
            // Multi-line input (if/for/def/class blocks)
            while (mp_repl_continue_with_input(vstr_null_terminated_str(&line))) {
                vstr_add_byte(&line, '\n');
                ret = readline(&line, "... ");
                if (ret == CHAR_CTRL_C) {
                    mp_hal_stdout_tx_str("\r\n");
                    goto input_restart;
                } else if (ret == CHAR_CTRL_D) {
                    break;
                }
            }
        }

        mp_hal_stdio_mode_orig();
        {
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_lexer_t *lex = mp_lexer_new_from_str_len(
                    MP_QSTR__lt_stdin_gt_,
                    vstr_str(&line), vstr_len(&line), 0);
                qstr source_name = lex->source_name;
                mp_parse_tree_t parse_tree = mp_parse(lex, parse_input_kind);
                mp_obj_t module_fun = mp_compile(&parse_tree, source_name,
                    parse_input_kind == MP_PARSE_SINGLE_INPUT);
                mp_call_function_0(module_fun);
                nlr_pop();
            } else {
                if (mp_obj_is_subclass_fast(
                        MP_OBJ_FROM_PTR(((mp_obj_base_t *)nlr.ret_val)->type),
                        MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
                    break;
                }
                mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
            }
        }
        mp_hal_stdio_mode_raw();
        continue;
    input_restart:
        continue;
    }

    vstr_clear(&line);
    mp_hal_stdio_mode_orig();
    mp_hal_stdout_tx_str("Bye!\r\n");
}

// Execute a string of Python code (for -c option). Returns 0 on success.
static int do_str(const char *str) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(
            MP_QSTR__lt_stdin_gt_, str, strlen(str), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, false);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        return 1;
    }
}

// Execute a .py script file. Returns 0 on success, non-zero on error.
static int do_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        printf("Error: cannot open file '%s'\n", filename);
        return 1;
    }
    fclose(f);
    int ret = pyexec_file(filename);
    return ret == 0 ? 1 : 0; // pyexec_file returns 1 on success
}

// Initialise the VM and populate sys.argv.
static void vm_init(int argc, char **argv) {
    if (heap == NULL) {
        unsigned long avail = (unsigned long)AvailMem(MEMF_ANY);
        if (heap_size > avail * 80 / 100) {
            printf("Warning: requesting %luKB but only %luKB available\n",
                   heap_size / 1024, avail / 1024);
        }
        heap = (char *)AllocMem(heap_size, MEMF_ANY | MEMF_CLEAR);
        if (heap == NULL) {
            printf("Error: cannot allocate %luKB heap\n", heap_size / 1024);
            exit(1);
        }
    }
    gc_init(heap, heap + heap_size);
    mp_stack_ctrl_init();
    mp_stack_set_top(stack_top);
    mp_init();
    // Mount POSIX VFS at root so open() works
    {
        mp_obj_t vfs_obj = MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(
            &mp_type_vfs_posix, 0, 0, NULL);
        mp_obj_t mount_args[2] = { vfs_obj, MP_OBJ_NEW_QSTR(MP_QSTR__slash_) };
        mp_vfs_mount(2, mount_args, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
    }
    // Populate sys.argv with argv[1:] (mp_init already created the empty list)
    for (int i = 1; i < argc; i++) {
        mp_obj_list_append(mp_sys_argv,
            MP_OBJ_NEW_QSTR(qstr_from_str(argv[i])));
    }
}

int main(int argc, char **argv) {
    // Ensure we have enough stack (swap if shell didn't provide enough)
    ensure_stack();

    // Set stack_top to the actual stack in use.
    // If we swapped, main()'s frame is on the old shell stack —
    // use the top of our new stack instead.
    if (stack_swapped) {
        stack_top = new_stack + MIN_STACK_SIZE;
    } else {
        int stack_dummy;
        stack_top = (char *)&stack_dummy;
    }

    // Save the shell's current directory lock so we can restore it on exit.
    original_dir = CurrentDir(0);
    CurrentDir(original_dir);

    // Parse -m <size_kb> option (must be first)
    if (argc >= 3 && strcmp(argv[1], "-m") == 0) {
        heap_size = (unsigned long)atol(argv[2]) * 1024;
        if (heap_size == 0) {
            heap_size = MICROPY_HEAP_SIZE;
        }
        argc -= 2;
        argv += 2;
    }

    int ret = 0;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        vm_init(argc - 1, argv + 1); // sys.argv = ['-c', ...]  skipping the code string
        ret = do_str(argv[2]);
        gc_sweep_all();
        mp_deinit();
    } else if (argc > 1) {
        vm_init(argc, argv);
        ret = do_file(argv[1]);
        gc_sweep_all();
        mp_deinit();
    } else {
        vm_init(argc, argv);
        do_repl();
        gc_sweep_all();
        mp_deinit();
    }

    extern void amissl_cleanup(void);
    amissl_cleanup();

    if (heap != NULL) {
        FreeMem(heap, heap_size);
        heap = NULL;
    }

    CurrentDir(original_dir);

    // Restore original stack and free our 64KB allocation.
    // _exit() is lightweight (no libamisslauto destructors) so the
    // shell's original stack is sufficient.
    restore_stack();
    _exit(ret);
}

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

// mp_lexer_new_from_file() is provided by READER_VFS

// open() is provided by VFS_POSIX (mp_vfs_open)

// mp_import_stat() is provided by VFS_POSIX

void nlr_jump_fail(void *val) {
    (void)val;
    mp_hal_stdio_mode_orig();
    printf("FATAL: uncaught NLR\n");
    extern void amissl_cleanup(void);
    amissl_cleanup();
    if (heap != NULL) {
        FreeMem(heap, heap_size);
        heap = NULL;
    }
    CurrentDir(original_dir);
    _exit(1);
}

#ifndef NDEBUG
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)func;
    mp_hal_stdio_mode_orig();
    printf("Assertion '%s' failed, at file %s:%d\n", expr, file, line);
    extern void amissl_cleanup(void);
    amissl_cleanup();
    if (heap != NULL) {
        FreeMem(heap, heap_size);
        heap = NULL;
    }
    CurrentDir(original_dir);
    _exit(1);
}
#endif
