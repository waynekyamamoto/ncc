// main.c — Compiler driver
// Usage: ncc [options] <file.c>
// Outputs: ARM64 assembly to stdout or produces object/executable via system tools
#include "cc.h"
#include <libgen.h>
#include <unistd.h>
#include <mach-o/dyld.h>

// Global options
bool opt_fpic = true;     // macOS always uses PIC
bool opt_fcommon = true;
char *base_file;

static StringArray input_files_list;
static StringArray tmpfiles;
static StringArray extra_includes;
static char *opt_o;
static bool opt_S;  // Output assembly only
static bool opt_c;  // Compile only, don't link
static bool opt_E;  // Preprocess only

static void usage(int status) {
  fprintf(stderr, "Usage: ncc [options] <file...>\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  -o <file>    Output file\n");
  fprintf(stderr, "  -S           Output assembly\n");
  fprintf(stderr, "  -c           Compile only\n");
  fprintf(stderr, "  -E           Preprocess only\n");
  fprintf(stderr, "  -I <dir>     Add include path\n");
  fprintf(stderr, "  -D <macro>   Define macro\n");
  fprintf(stderr, "  -U <macro>   Undefine macro\n");
  fprintf(stderr, "  -include <file> Include file\n");
  exit(status);
}

static char *create_tmpfile(void) {
  char *path = strdup("/tmp/ncc-XXXXXX");
  int fd = mkstemp(path);
  if (fd == -1)
    error("mkstemp failed: %s", strerror(errno));
  close(fd);
  strarray_push(&tmpfiles, path);
  return path;
}

static void cleanup(void) {
  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
}

// Run an external command
static void run_cmd(StringArray *cmd) {
  // Build command string for system()
  char *buf;
  size_t buflen;
  FILE *fp = open_memstream(&buf, &buflen);
  for (int i = 0; i < cmd->len; i++) {
    if (i > 0) fputc(' ', fp);
    fprintf(fp, "%s", cmd->data[i]);
  }
  fclose(fp);

  if (system(buf) != 0)
    error("command failed: %s", buf);
  free(buf);
}

// Filter GAS-only directives that macOS `as` doesn't support.
// Handles the Linux kernel's use of: .subsection/.previous, .irp/.endr,
// .macro/.endm (with subsequent invocations), .pushsection/.popsection.
// Also strips \( GAS macro escapes from remaining lines.
static char *filter_asm(char *input) {
  FILE *in = fopen(input, "r");
  if (!in) return input;

  char *out_path = create_tmpfile();
  FILE *out = fopen(out_path, "w");
  if (!out) { fclose(in); return input; }

  // Track names of GAS macros defined with .macro (and then skipped).
  // Subsequent invocations of these macros are also skipped.
  char *skipped_macros[32];
  int n_skipped = 0;

  typedef enum { ST_NORMAL, ST_SUBSECTION, ST_IREP, ST_MACRO, ST_PUSHSECT } State;
  State state = ST_NORMAL;

  char *line = NULL;
  size_t cap = 0;

  while (getline(&line, &cap, in) != -1) {
    char *t = line;
    while (*t == ' ' || *t == '\t') t++;

    // Extract first word of line for matching
    char word[64] = {0};
    int wlen = 0;
    for (char *p = t; *p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r'; p++)
      if (wlen < 63) word[wlen++] = *p;
    word[wlen] = '\0';

    if (state != ST_NORMAL) {
      // Check for block-ending directives
      bool end_block = false;
      if (state == ST_SUBSECTION && strcmp(word, ".previous") == 0) end_block = true;
      if (state == ST_IREP      && strcmp(word, ".endr")     == 0) end_block = true;
      if (state == ST_MACRO     && strcmp(word, ".endm")     == 0) end_block = true;
      if (state == ST_PUSHSECT  && strcmp(word, ".popsection") == 0) end_block = true;
      if (end_block) state = ST_NORMAL;
      fputc('\n', out); // preserve line count
      continue;
    }

    // NORMAL state — check for block-starting directives
    if (strcmp(word, ".subsection") == 0) {
      state = ST_SUBSECTION;
      fputc('\n', out);
      continue;
    }
    if (strcmp(word, ".irp") == 0 || strcmp(word, ".irpc") == 0) {
      state = ST_IREP;
      fputc('\n', out);
      continue;
    }
    if (strcmp(word, ".pushsection") == 0) {
      state = ST_PUSHSECT;
      fputc('\n', out);
      continue;
    }
    if (strcmp(word, ".macro") == 0) {
      // Extract macro name (second word after .macro)
      char *p = t + 6; // skip ".macro"
      while (*p == ' ' || *p == '\t') p++;
      char mname[64] = {0};
      int ml = 0;
      while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        if (ml < 63) mname[ml++] = *p++;
      mname[ml] = '\0';
      if (ml > 0 && n_skipped < 32)
        skipped_macros[n_skipped++] = strdup(mname);
      state = ST_MACRO;
      fputc('\n', out);
      continue;
    }
    if (strcmp(word, ".purgem") == 0) {
      // Always skip .purgem for macros we skipped (avoids "macro not defined" error)
      fputc('\n', out);
      continue;
    }

    // Check if this line is an invocation of a skipped macro
    bool is_macro_call = false;
    for (int i = 0; i < n_skipped; i++) {
      if (strcmp(word, skipped_macros[i]) == 0) {
        is_macro_call = true;
        break;
      }
    }
    if (is_macro_call) {
      fputc('\n', out);
      continue;
    }

    // Normal line — output it, stripping GAS macro escape \( -> (
    for (char *p = line; *p; p++) {
      if (p[0] == '\\' && p[1] == '(') {
        fputc('(', out);
        p++;
      } else {
        fputc(*p, out);
      }
    }
  }

  // Free skipped macro names
  for (int i = 0; i < n_skipped; i++) free(skipped_macros[i]);
  free(line);
  fclose(in);
  fclose(out);
  return out_path;
}

// Assemble a .s file to .o
static void assemble(char *input, char *output) {
  char *filtered = filter_asm(input);
  StringArray cmd = {};
  strarray_push(&cmd, "as");
  strarray_push(&cmd, "-o");
  strarray_push(&cmd, output);
  strarray_push(&cmd, filtered);
  run_cmd(&cmd);
}

// Linker flags collected from command line
static StringArray ld_extra_flags;

// Link .o files to executable
static void link_files(StringArray *inputs, char *output) {
  StringArray cmd = {};
  strarray_push(&cmd, "ld");
  strarray_push(&cmd, "-o");
  strarray_push(&cmd, output);
  strarray_push(&cmd, "-lSystem");
  strarray_push(&cmd, "-syslibroot");
  strarray_push(&cmd, "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk");
  strarray_push(&cmd, "-arch");
  strarray_push(&cmd, "arm64");
  strarray_push(&cmd, "-e");
  strarray_push(&cmd, "_main");

  for (int i = 0; i < inputs->len; i++)
    strarray_push(&cmd, inputs->data[i]);

  for (int i = 0; i < ld_extra_flags.len; i++)
    strarray_push(&cmd, ld_extra_flags.data[i]);

  run_cmd(&cmd);
}

// Compile a single file
static void compile(char *input_path, char *output_path) {
  // Make labels unique per compilation unit to avoid Mach-O symbol
  // collisions (.L. labels are NOT stripped on Mach-O like on ELF).
  {
    unsigned hash = 0;
    for (char *p = input_path; *p; p++)
      hash = hash * 31 + (unsigned char)*p;
    label_cnt = (int)(hash % 900000) * 1000;
    gvar_cnt = label_cnt;
  }

  Token *tok = tokenize_file(input_path);
  if (!tok)
    error("%s: %s", input_path, strerror(errno));

  // Prepend -include files (processed first, as if #include'd at top of file)
  for (int j = extra_includes.len - 1; j >= 0; j--) {
    Token *inc = tokenize_file(extra_includes.data[j]);
    if (!inc)
      error("-include %s: %s", extra_includes.data[j], strerror(errno));
    Token *t = inc;
    while (t->next->kind != TK_EOF)
      t = t->next;
    t->next = tok;
    tok = inc;
  }

  tok = preprocess(tok);

  if (opt_E) {
    // Print preprocessed output
    FILE *fp = output_path ? fopen(output_path, "w") : stdout;
    for (Token *t = tok; t->kind != TK_EOF; t = t->next) {
      if (t->at_bol)
        fprintf(fp, "\n");
      if (t->has_space && !t->at_bol)
        fputc(' ', fp);
      fprintf(fp, "%.*s", t->len, t->loc);
    }
    fprintf(fp, "\n");
    if (fp != stdout)
      fclose(fp);
    return;
  }

  Obj *prog = parse(tok);

  FILE *fp = fopen(output_path, "w");
  if (!fp)
    error("cannot open output file: %s: %s", output_path, strerror(errno));

  codegen(prog, fp);
  fclose(fp);
}

static char *replace_ext(char *path, char *ext) {
  char *p = strrchr(path, '.');
  if (p) {
    int len = p - path;
    return format("%.*s%s", len, path, ext);
  }
  return format("%s%s", path, ext);
}

int main(int argc, char **argv) {
  atexit(cleanup);
  init_macros();

  // Add compiler-provided headers first (stdarg.h, stddef.h, etc.)
  // These must come before user -I paths and system paths.
  {
    char exe_path[1024];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) == 0) {
      char *dir = dirname(strdup(exe_path));
      add_include_path(format("%s/include", dir));
    }
  }

  // Parse command-line arguments
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help"))
      usage(0);

    if (!strcmp(argv[i], "-o")) {
      if (++i >= argc)
        usage(1);
      opt_o = argv[i];
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }

    if (!strncmp(argv[i], "-I", 2)) {
      if (argv[i][2])
        add_include_path(argv[i] + 2);
      else if (++i < argc)
        add_include_path(argv[i]);
      continue;
    }

    if (!strncmp(argv[i], "-D", 2)) {
      char *arg;
      if (argv[i][2])
        arg = argv[i] + 2;
      else if (++i < argc)
        arg = argv[i];
      else
        usage(1);

      char *eq = strchr(arg, '=');
      if (eq) {
        *eq = '\0';
        define_macro(arg, eq + 1);
      } else {
        define_macro(arg, "1");
      }
      continue;
    }

    if (!strncmp(argv[i], "-U", 2)) {
      char *arg = NULL;
      if (argv[i][2])
        arg = argv[i] + 2;
      else if (++i < argc)
        arg = argv[i];
      else
        usage(1);
      undef_macro(arg);
      continue;
    }

    if (!strcmp(argv[i], "-include") || !strcmp(argv[i], "-include-pch")) {
      if (++i >= argc) usage(1);
      strarray_push(&extra_includes, argv[i]);
      continue;
    }

    // Library/linker flags (pass through to linker)
    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-L", 2)) {
      strarray_push(&ld_extra_flags, argv[i]);
      continue;
    }
    if (!strcmp(argv[i], "-framework")) {
      strarray_push(&ld_extra_flags, argv[i]);
      if (++i < argc)
        strarray_push(&ld_extra_flags, argv[i]);
      continue;
    }
    if (!strncmp(argv[i], "-Wl,", 4)) {
      // Split -Wl,arg1,arg2 into separate ld args
      char *args = strdup(argv[i] + 4);
      char *p = strtok(args, ",");
      while (p) {
        strarray_push(&ld_extra_flags, strdup(p));
        p = strtok(NULL, ",");
      }
      continue;
    }

    // Ignore common flags we don't support
    if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "-g") ||
        !strcmp(argv[i], "-O0") || !strcmp(argv[i], "-O1") ||
        !strcmp(argv[i], "-O2") || !strcmp(argv[i], "-O3") ||
        !strcmp(argv[i], "-Os") || !strcmp(argv[i], "-Wall") ||
        !strcmp(argv[i], "-Werror") || !strcmp(argv[i], "-Wextra") ||
        !strcmp(argv[i], "-pedantic") ||
        !strncmp(argv[i], "-W", 2) ||
        !strncmp(argv[i], "-O", 2) ||
        !strncmp(argv[i], "-std=", 5) ||
        !strncmp(argv[i], "-f", 2) ||
        !strncmp(argv[i], "-m", 2) ||
        !strcmp(argv[i], "-pipe") ||
        !strcmp(argv[i], "-MD") ||
        !strcmp(argv[i], "-MF") ||
        !strcmp(argv[i], "-MT") ||
        !strcmp(argv[i], "-MQ") ||
        !strcmp(argv[i], "-MP")) {
      // Some of these take an argument
      if (!strcmp(argv[i], "-MF") || !strcmp(argv[i], "-MT") ||
          !strcmp(argv[i], "-MQ"))
        i++; // skip argument
      continue;
    }

    if (argv[i][0] == '-') {
      // Unknown flag — ignore with warning
      fprintf(stderr, "ncc: warning: unknown flag '%s'\n", argv[i]);
      continue;
    }

    // Input file
    strarray_push(&input_files_list, argv[i]);
  }

  // Add system include paths AFTER user -I paths so that -I overrides system headers.
  // This is standard GCC/Clang behavior: user -I paths shadow system paths.
  add_include_path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
  add_include_path("/usr/local/include");
  add_include_path("/usr/include");

  if (input_files_list.len == 0)
    error("no input files");

  base_file = input_files_list.data[0];

  if (opt_E) {
    // Preprocess only
    for (int i = 0; i < input_files_list.len; i++)
      compile(input_files_list.data[i], opt_o);
    return 0;
  }

  // Compile each file
  StringArray obj_files = {};

  for (int i = 0; i < input_files_list.len; i++) {
    char *input = input_files_list.data[i];
    int len = strlen(input);

    // .o and .a files go directly to the linker
    if ((len > 2 && !strcmp(input + len - 2, ".o")) ||
        (len > 2 && !strcmp(input + len - 2, ".a"))) {
      strarray_push(&obj_files, input);
      continue;
    }

    if (opt_S) {
      // Output assembly
      char *output = opt_o ? opt_o : replace_ext(input, ".s");
      compile(input, output);
    } else if (opt_c) {
      // Compile to object
      char *asm_file = create_tmpfile();
      compile(input, asm_file);
      char *output = opt_o ? opt_o : replace_ext(input, ".o");
      assemble(asm_file, output);
    } else {
      // Compile, assemble, link
      char *asm_file = create_tmpfile();
      compile(input, asm_file);
      char *obj_file = create_tmpfile();
      assemble(asm_file, obj_file);
      strarray_push(&obj_files, obj_file);
    }
  }

  // Link if not -S or -c
  if (!opt_S && !opt_c && obj_files.len > 0) {
    char *output = opt_o ? opt_o : "a.out";
    link_files(&obj_files, output);
  }

  return 0;
}
