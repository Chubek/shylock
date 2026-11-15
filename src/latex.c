#include "shylock.h"
#include "latex.h"
#include "mulib-latex.h"
#include <stdio.h>
#include <string.h>

/* Only ABI/lifetime adaptation. On1x owns templates, substitution and layout.
   No raw TeX, filesystem output, process launch or printer API is installed. */
static mulib_latex_t document;
static int failed;

int64_t shy_latex_close(void)
{
    mulib_latex_free(&document); failed = 0; return 0;
}

int64_t shy_latex_begin(const char *title)
{
    shy_latex_close();
    failed = strlen(title) > 8192 || !mulib_latex_begin(&document, title);
    return !failed;
}

int64_t shy_latex_append(int64_t level, const char *text)
{
    size_t n = strlen(text);
    if (failed || n > 65536 || level < 0 || level > 3) { failed = 1; return 0; }
    int ok = level ? mulib_latex_section(&document, (unsigned)level, text) :
        mulib_latex_paragraph(&document, text, n);
    if (!ok || document.source.length > SHY_PAGE_LIMIT) failed = 1;
    return !failed;
}

int64_t shy_latex_finish(void)
{
    if (failed || !mulib_latex_finish(&document) || document.source.length > SHY_PAGE_LIMIT) {
        failed = 1; return 0;
    }
    return 1;
}

const char *shy_latex_digest(void)
{
    static char digest[65];
    if (failed || document.state != 2) return "";
    shy_digest(document.source.data, document.source.length, digest);
    return digest;
}

int64_t shy_latex_publish(void)
{
    if (failed || document.state != 2) return shy_error("LaTeX document is not finished");
    if (fwrite(document.source.data, 1, document.source.length, stdout) != document.source.length || fflush(stdout))
        return shy_error("cannot print LaTeX report");
    return 0;
}
