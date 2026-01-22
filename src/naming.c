#include "naming.h"
#include "appmap.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * Extract readable app name from class regex pattern
 * Similar to clean_class_name in ui.c but standalone
 */
static char *extract_app_name(const char *regex) {
    if (!regex || regex[0] == '\0') return NULL;
    
    char buf[64] = "";
    const char *p = regex;
    size_t o = 0;
    
    /* skip leading ^ */
    if (*p == '^') p++;
    /* skip leading ( */
    if (*p == '(') p++;
    /* skip [Xx] case patterns */
    if (*p == '[' && p[1] && p[2] == ']') {
        buf[o++] = (p[1] >= 'a' && p[1] <= 'z') ? p[1] - 32 : p[1];
        p += 3;
    }
    
    while (*p && o < sizeof(buf) - 1) {
        /* stop at regex special chars */
        if (*p == '$' || *p == ')' || *p == '|') break;
        /* handle character classes */
        if (*p == '[') {
            if (p[1] && p[1] != ']') buf[o++] = p[1];
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
            continue;
        }
        /* skip quantifiers */
        if (*p == '+' || *p == '*' || *p == '?') { p++; continue; }
        if (*p == '.' && p[1] == '+') { p += 2; continue; }
        /* unescape */
        if (*p == '\\' && p[1]) {
            p++;
            buf[o++] = *p++;
            continue;
        }
        buf[o++] = *p++;
    }
    buf[o] = '\0';
    
    if (buf[0] == '\0' && regex[0]) {
        /* fallback: extract first word-like chars */
        p = regex;
        o = 0;
        while (*p && o < sizeof(buf) - 1) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') {
                buf[o++] = *p;
            } else if (o > 0) {
                break;
            }
            p++;
        }
        buf[o] = '\0';
    }
    
    /* capitalize first letter */
    if (buf[0] >= 'a' && buf[0] <= 'z') {
        buf[0] -= 32;
    }
    
    return buf[0] != '\0' ? strdup(buf) : NULL;
}

/**
 * Generate a suggested human-readable name for a rule
 */
char *naming_suggest_name(struct rule *r) {
    if (!r) return strdup("(unknown)");
    
    /* If rule already has a good name, use it */
    if (r->name && strlen(r->name) > 5 && strchr(r->name, '-') == NULL) {
        /* Name doesn't look auto-generated */
        return strdup(r->name);
    }
    
    char *suggested = NULL;
    
    /* Try class pattern first */
    if (r->match.class_re) {
        suggested = extract_app_name(r->match.class_re);
        if (suggested) return suggested;
    }
    
    /* Try title pattern */
    if (r->match.title_re) {
        suggested = extract_app_name(r->match.title_re);
        if (suggested) return suggested;
    }
    
    /* Try initial_class */
    if (r->match.initial_class_re) {
        suggested = extract_app_name(r->match.initial_class_re);
        if (suggested) return suggested;
    }
    
    /* Fallback to rule name if it exists */
    if (r->name) {
        return strdup(r->name);
    }
    
    /* Last resort */
    return strdup("(unnamed)");
}

/**
 * Check if display_name (derived) differs from actual name field
 */
int naming_has_mismatch(struct rule *r) {
    if (!r) return 0;
    
    const char *display = r->display_name ? r->display_name : "(unnamed)";
    const char *actual = r->name ? r->name : "";
    
    /* Mismatch if they're substantially different */
    return strcmp(display, actual) != 0;
}

/**
 * Update a rule's name field
 */
void naming_set_rule_name(struct rule *r, const char *new_name) {
    if (!r) return;
    
    free(r->name);
    r->name = (new_name && strlen(new_name) > 0) ? strdup(new_name) : NULL;
}

/**
 * Get best display name (prefer user-assigned name, fallback to display_name)
 */
const char *naming_get_display_name(struct rule *r) {
    if (!r) return "(unknown)";
    
    /* If name is set and looks user-assigned (not Rule-XXX), use it */
    if (r->name && strlen(r->name) > 5) {
        /* Check if it looks auto-generated (Rule-123 pattern) */
        if (strncmp(r->name, "Rule-", 5) != 0) {
            return r->name;
        }
    }
    
    /* Fall back to display_name */
    return r->display_name ? r->display_name : "(unnamed)";
}
