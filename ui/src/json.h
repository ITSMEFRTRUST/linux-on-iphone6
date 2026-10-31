/* json.c — a small JSON reader (whole document → tree) for the internet apps. */
#pragma once
#include <stdbool.h>

enum json_type { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct json {
	enum json_type type;
	double num;		/* J_NUM, J_BOOL (0/1) */
	char *str;		/* J_STR (UTF-8) */
	char *key;		/* member name inside an object */
	struct json **kids;	/* J_ARR / J_OBJ */
	int n;
} json_t;

json_t *json_parse(const char *s);	/* NULL = not JSON */
void json_free(json_t *j);
json_t *json_get(const json_t *obj, const char *key);	/* NULL if missing / not an object */
json_t *json_at(const json_t *arr, int i);
int json_len(const json_t *arr);
double json_num(const json_t *j, double def);
const char *json_str(const json_t *j, const char *def);
