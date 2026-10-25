/* spore command line: helpers shared by the subcommands.
 * SPDX-License-Identifier: MIT */
#ifndef SPORE_CLI_H
#define SPORE_CLI_H

/* Inside an argument loop over argv[i], with `a` the argument and `v` the
 * next one: CLI_FLAG matches any of the names; CLI_ARG also consumes `v`,
 * exiting with status 2 if it is missing. */
#define CLI_FLAG(...) cli_is(a, (const char *const[]){__VA_ARGS__, 0})
#define CLI_ARG(...) (CLI_FLAG(__VA_ARGS__) && (v || cli_missing(a)) && ++i)

int cli_is(const char *a, const char *const *names);
int cli_missing(const char *flag);
/* `v` as a decimal integer in [lo, hi], or exit 2 naming `flag`. */
long cli_long(const char *flag, const char *v, long lo, long hi);
void cli_unknown(const char *cmd, const char *arg);
void cli_too_many(const char *flag, int max);

int serve_main(int argc, char **argv);

#endif
