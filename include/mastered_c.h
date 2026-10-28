#ifndef MASTERED_C_H
#define MASTERED_C_H

#if defined(_WIN32)
#if defined(MASTERED_BUILDING)
#define MASTERED_API __declspec(dllexport)
#else
#define MASTERED_API __declspec(dllimport)
#endif
#else
#define MASTERED_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mastered_engine mastered_engine;

MASTERED_API const char* mastered_version(void);
MASTERED_API mastered_engine* mastered_create(void);
MASTERED_API void mastered_destroy(mastered_engine* engine);
MASTERED_API int mastered_set_option(mastered_engine* engine, const char* key, double value);
MASTERED_API int mastered_add_reference(mastered_engine* engine, const char* path);
MASTERED_API void mastered_clear_references(mastered_engine* engine);
MASTERED_API int mastered_run(mastered_engine* engine, const char* input_path, const char* output_path);
MASTERED_API int mastered_get_metric(mastered_engine* engine, const char* key, double* value);
MASTERED_API const char* mastered_result_json(mastered_engine* engine);
MASTERED_API const char* mastered_last_error(mastered_engine* engine);

#ifdef __cplusplus
}
#endif

#endif
