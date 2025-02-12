/*
 * Copyright The rpminspect Project Authors
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <assert.h>

#include "rpminspect.h"

/*
 * Get the basename of the shell from the #! line of a script.
 * Return the name if it's in our list of shells to use.
 * Return if invalid or not found.
 * Caller is responsible for freeing the returned string.
 */
static char *get_shell(const struct rpminspect *ri, const char *fullpath)
{
    char *shell = NULL;
    FILE *fp = NULL;
    char *buf = NULL;
    char *walk = NULL;
    char *start = NULL;
    size_t len;
    int r = 0;

    assert(ri != NULL);
    assert(ri->shells != NULL);
    assert(fullpath != NULL);

    fp = fopen(fullpath, "r");

    if (fp == NULL) {
        warn("*** fopen");
        return NULL;
    }

    r = getline(&buf, &len, fp);
    start = buf;

    if (fclose(fp) == -1) {
        warn("*** fclose");
    }

    if (r == -1) {
        warn("*** getline");
    } else if (!strncmp(buf, "#!", 2)) {
        /* trim newlines */
        buf[strcspn(buf, "\n")] = '\0';

        /* shift over to start to the shell (skip #! and spaces) */
        while (*buf != '/' && *buf != ' ' && *buf != '\0') {
            buf++;
        }

        /* get just the executable name (trims options) */
        walk = buf;

        while (*walk != ' ' && *walk != '\0') {
            walk++;
        }

        *walk = '\0';
        walk = basename(buf);

        /* is it a shell we care about? */
        if (list_contains(ri->shells, walk)) {
            shell = strdup(walk);
        }
    }

    free(start);
    return shell;
}

static bool shellsyntax_driver(struct rpminspect *ri, rpmfile_entry_t *file)
{
    bool result = true;
    const char *arch = NULL;
    const char *type = NULL;
    char *shell = NULL;
    char *before_shell = NULL;
    int exitcode = -1;
    char *before_errors = NULL;
    int before_exitcode = -1;
    char *errors = NULL;
    char *tmp = NULL;
    bool extglob = false;
    struct result_params params;

    /* Ignore files in the SRPM */
    if (headerIsSource(file->rpm_header)) {
        return true;
    }

    /* Get the mime type of the file */
    type = get_mime_type(ri, file);

    if (!strprefix(type, "text/")) {
        return true;
    }

    /* We need the architecture for reporting */
    arch = get_rpm_header_arch(file->rpm_header);

    /* Get the shell from the #! line */
    shell = get_shell(ri, file->fullpath);

    if (!shell) {
        return true;
    }

    DEBUG_PRINT("shell=|%s|\n", shell);

    /* Set up the result parameters */
    init_result_params(&params);
    params.header = NAME_SHELLSYNTAX;
    params.arch = arch;
    params.file = file->localpath;

    if (file->peer_file) {
        before_shell = get_shell(ri, file->peer_file->fullpath);
        DEBUG_PRINT("before_shell=|%s|\n", before_shell);

        if (!before_shell) {
            xasprintf(&params.msg, _("%s is a shell script but was not before on %s"), file->localpath, arch);
        } else if (strcmp(shell, before_shell)) {
            xasprintf(&params.msg, _("%s is a %s script but was a %s script before on %s"), file->localpath, shell, before_shell, arch);
        }

        if (params.msg) {
            params.severity = RESULT_INFO;
            params.waiverauth = NOT_WAIVABLE;
            params.remedy = REMEDY_SHELLSYNTAX_GAINED_SHELL;
            params.verb = VERB_OK;
            params.noun = _("changed shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
        }
    }

    /* Run with -n and capture results */
    errors = run_cmd(&exitcode, ri->worksubdir, shell, "-n", file->fullpath, NULL);
    DEBUG_PRINT("exitcode=%d, errors=|%s|\n", exitcode, errors);

    if (before_shell) {
        before_errors = run_cmd(&before_exitcode, ri->worksubdir, before_shell, "-n", file->peer_file->fullpath, NULL);
        DEBUG_PRINT("before_exitcode=%d, before_errors=|%s|\n", before_exitcode, before_errors);

        /* remove the working directory prefix */
        tmp = strreplace(before_errors, file->peer_file->fullpath, file->peer_file->localpath);
        free(before_errors);
        before_errors = tmp;
    }

    /* Special check for GNU bash, try with extglob */
    if (exitcode && !strcmp(shell, "bash")) {
        free(errors);
        errors = run_cmd(&exitcode, ri->worksubdir, shell, "-n", "-O", "extglob", file->fullpath, NULL);
        DEBUG_PRINT("exitcode=%d, errors=|%s|\n", exitcode, errors);

        if (!exitcode) {
            extglob = true;
            result = false;
        }
    }

    /* remove the working directory prefix */
    tmp = strreplace(errors, file->fullpath, file->localpath);
    free(errors);
    errors = tmp;

    /* Report */
    if (before_shell) {
        if ((!before_exitcode || before_errors == NULL) && (exitcode || errors)) {
            xasprintf(&params.msg, _("%s is no longer a valid %s script on %s"), file->localpath, shell, arch);
            params.severity = RESULT_BAD;
            params.waiverauth = WAIVABLE_BY_ANYONE;
            params.remedy = REMEDY_SHELLSYNTAX_BAD;
            params.details = errors;
            params.verb = VERB_FAILED;
            params.noun = _("invalid shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
            result = false;
        } else if ((before_exitcode || before_errors) && (!exitcode && errors == NULL)) {
            xasprintf(&params.msg, _("%s became a valid %s script on %s"), file->localpath, shell, arch);

            if (extglob) {
                params.msg = strappend(params.msg, _(". The script fails with '-n' but passes with '-O extglob'; be sure 'shopt extglob' is set in the script."), NULL);
            }

            params.severity = RESULT_INFO;
            params.waiverauth = NOT_WAIVABLE;
            params.details = before_errors;
            params.remedy = NULL;
            params.verb = VERB_OK;
            params.noun = _("valid shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
        } else if ((before_exitcode || before_errors) && (exitcode || errors)) {
            xasprintf(&params.msg, _("%s is not a valid %s script on %s"), file->localpath, shell, arch);
            params.severity = RESULT_BAD;
            params.waiverauth = WAIVABLE_BY_ANYONE;
            params.remedy = REMEDY_SHELLSYNTAX_BAD;
            params.details = errors;
            params.verb = VERB_FAILED;
            params.noun = _("invalid shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
            result = false;
        }
    } else {
        if ((!exitcode || errors == NULL) && extglob) {
            xasprintf(&params.msg, _("%s fails with '-n' but passes with '-O extglob'; be sure 'shopt extglob' is set in the script on %s"), file->localpath, arch);
            params.severity = RESULT_INFO;
            params.waiverauth = NOT_WAIVABLE;
            params.details = NULL;
            params.remedy = NULL;
            params.verb = VERB_OK;
            params.noun = _("valid shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
        } else if (exitcode || errors) {
            xasprintf(&params.msg, _("%s is not a valid %s script on %s"), file->localpath, shell, arch);
            params.severity = RESULT_BAD;
            params.waiverauth = WAIVABLE_BY_ANYONE;
            params.details = errors;
            params.remedy = REMEDY_SHELLSYNTAX_BAD;
            params.noun = _("invalid shell script ${FILE} on ${ARCH}");
            add_result(ri, &params);
            free(params.msg);
            result = false;
        }
    }

    free(shell);
    free(before_shell);
    return result;
}

/*
 * Main driver for the 'shellsyntax' inspection.
 */
bool inspect_shellsyntax(struct rpminspect *ri)
{
    bool result;
    struct result_params params;

    assert(ri != NULL);

    result = foreach_peer_file(ri, NAME_SHELLSYNTAX, shellsyntax_driver);

    if (result) {
        init_result_params(&params);
        params.severity = RESULT_OK;
        params.header = NAME_SHELLSYNTAX;
        params.verb = VERB_OK;
        add_result(ri, &params);
    }

    return result;
}
