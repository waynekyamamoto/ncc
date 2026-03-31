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

// Assemble a .s file to .o
static void assemble(char *input, char *output) {
  StringArray cmd = {};
  strarray_push(&cmd, "as");
  strarray_push(&cmd, "-o");
  strarray_push(&cmd, output);
  strarray_push(&cmd, input);
  run_cmd(&cmd);
}

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
  }

  Token *tok = tokenize_file(input_path);
  if (!tok)
    error("%s: %s", input_path, strerror(errno));

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

  // Add default include paths
  // Compiler-provided headers (stdarg.h, stddef.h, etc.) — search first
  // Try to find the include dir relative to the executable
  {
    char exe_path[1024];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) == 0) {
      char *dir = dirname(strdup(exe_path));
      add_include_path(format("%s/include", dir));
    }
  }
  add_include_path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
  add_include_path("/usr/local/include");
  add_include_path("/usr/include");

  StringArray extra_includes = {};
  (void)extra_includes;

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

    if (!strcmp(argv[i], "-include")) {
      if (++i >= argc) usage(1);
      strarray_push(&extra_includes, argv[i]);
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

    // Library flags (pass through to linker)
    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-L", 2)) {
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
