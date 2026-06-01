#include "leash/download.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static size_t write_download(void *ptr, size_t size, size_t nmemb, void *userdata) {
  FILE *file = userdata;
  return fwrite(ptr, size, nmemb, file);
}

int vm_download_file(const char *url, const char *path) {
  static int curl_initialized = 0;
  if (!curl_initialized) {
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
      fprintf(stderr, "curl_global_init: %s\n", curl_easy_strerror(rc));
      return 1;
    }
    curl_initialized = 1;
  }

  char *tmp = NULL;
  if (asprintf(&tmp, "%s.download", path) < 0) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  FILE *file = fopen(tmp, "wb");
  if (!file) {
    fprintf(stderr, "open %s: %s\n", tmp, strerror(errno));
    free(tmp);
    return 1;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "curl_easy_init failed\n");
    fclose(file);
    unlink(tmp);
    free(tmp);
    return 1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "leash/0.1");

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  int close_rc = fclose(file);
  if (rc != CURLE_OK || close_rc != 0) {
    if (rc != CURLE_OK) {
      fprintf(stderr, "download %s: %s", url, curl_easy_strerror(rc));
      if (status) fprintf(stderr, " (HTTP %ld)", status);
      fputc('\n', stderr);
    } else {
      fprintf(stderr, "write %s: %s\n", tmp, strerror(errno));
    }
    unlink(tmp);
    free(tmp);
    return 1;
  }

  if (rename(tmp, path) != 0) {
    fprintf(stderr, "rename %s to %s: %s\n", tmp, path, strerror(errno));
    unlink(tmp);
    free(tmp);
    return 1;
  }

  free(tmp);
  return 0;
}
