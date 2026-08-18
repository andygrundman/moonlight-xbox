/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "http.h"
#include "errors.h"

#include <stdbool.h>
#include <string.h>
#include <curl/curl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const char *pCertFile = "./client.pem";
static const char *pKeyFile = "./key.pem";

static bool debug;
static struct curl_blob certBlob, keyBlob;

// The blobs are freed and reassigned by http_init while get_curl_handle may
// be reading them from another thread (host polling, pairing, app fetches).
#ifdef _WIN32
static SRWLOCK blob_lock = SRWLOCK_INIT;
#define BLOB_LOCK() AcquireSRWLockExclusive(&blob_lock)
#define BLOB_UNLOCK() ReleaseSRWLockExclusive(&blob_lock)
#endif

static void free_certblobs_unlocked(void) {
    if (certBlob.data) {
        free((void*)certBlob.data);
        certBlob.data = NULL;
        certBlob.len = 0;
        certBlob.flags = 0;
    }
    if (keyBlob.data) {
        free((void*)keyBlob.data);
        keyBlob.data = NULL;
        keyBlob.len = 0;
        keyBlob.flags = 0;
    }
}

void http_free_certblobs(void) {
    BLOB_LOCK();
    free_certblobs_unlocked();
    BLOB_UNLOCK();
}


static size_t _write_curl(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  PHTTP_DATA mem = (PHTTP_DATA)userp;

  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL)
    return 0;

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

static size_t _write_curl_binary(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t written = fwrite(contents, size, nmemb, userp);
    return written;
}

CURL* get_curl_handle() {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLENGINE_DEFAULT, 1L);
    curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
    curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    // CURL_BLOB_COPY means curl copies the blob during setopt, so the
    // buffers only need to stay alive while the lock is held.
    BLOB_LOCK();
    if (certBlob.data != NULL) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &certBlob);
    }
    if (keyBlob.data != NULL) {
        curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &keyBlob);
    }
    BLOB_UNLOCK();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    return curl;
}

static void* read_file_blob(const char* path, size_t* outLen) {
  FILE* fp = fopen(path, "rb");
  if (fp == NULL) return NULL;
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
  long size = ftell(fp);
  if (size <= 0) { fclose(fp); return NULL; }
  rewind(fp);
  void* buffer = malloc((size_t)size);
  if (buffer == NULL) { fclose(fp); return NULL; }
  size_t read = fread(buffer, 1, (size_t)size, fp);
  fclose(fp);
  if (read != (size_t)size) { free(buffer); return NULL; }
  *outLen = read;
  return buffer;
}

int http_init(const char* keyDirectory, int logLevel) {
  debug = logLevel >= 2;

  // The blobs are loaded once and then left alone: reloading would free
  // buffers another thread's get_curl_handle may be copying.
  BLOB_LOCK();
  bool loaded = certBlob.data != NULL && keyBlob.data != NULL;
  BLOB_UNLOCK();
  if (loaded)
    return GS_OK;

  char certificateFilePath[4096];
  snprintf(certificateFilePath, sizeof(certificateFilePath), "%s%s", keyDirectory, CERTIFICATE_FILE_NAME);

  char keyFilePath[4096];
  snprintf(keyFilePath, sizeof(keyFilePath), "%s%s", keyDirectory, KEY_FILE_NAME);

  size_t certLen = 0;
  void* certificateBuffer = read_file_blob(certificateFilePath, &certLen);
  if (certificateBuffer == NULL) return 1;

  size_t keyLen = 0;
  void* keyBuffer = read_file_blob(keyFilePath, &keyLen);
  if (keyBuffer == NULL) { free(certificateBuffer); return 1; }

  BLOB_LOCK();
  free_certblobs_unlocked();
  certBlob.data = certificateBuffer;
  certBlob.len = certLen;
  certBlob.flags = CURL_BLOB_COPY;
  keyBlob.data = keyBuffer;
  keyBlob.len = keyLen;
  keyBlob.flags = CURL_BLOB_COPY;
  BLOB_UNLOCK();

  return GS_OK;
}

int http_request(CURL* curl, char* url, PHTTP_DATA data) {
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_curl);

  if (debug)
    printf("Request %s\n", url);

  if (data->size > 0) {
    free(data->memory);
    data->memory = malloc(1);
    if(data->memory == NULL)
      return GS_OUT_OF_MEMORY;

    data->size = 0;
  }
  CURLcode res = curl_easy_perform(curl);

  if(res != CURLE_OK) {
    gs_error = curl_easy_strerror(res);
    return GS_FAILED;
  } else if (data->memory == NULL) {
    return GS_OUT_OF_MEMORY;
  }

  if (debug)
    printf("Response:\n%s\n\n", data->memory);

  return GS_OK;
}

int http_request_binary(CURL *curl, char* url, FILE *data) {
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_curl_binary);

    if (debug)
        printf("Request %s\n", url);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        gs_error = curl_easy_strerror(res);
        return GS_FAILED;
    }
    return GS_OK;
}

void http_cleanup(CURL *curl) {
  curl_easy_cleanup(curl);
}

PHTTP_DATA http_create_data() {
  PHTTP_DATA data = malloc(sizeof(HTTP_DATA));
  if (data == NULL)
    return NULL;

  data->memory = malloc(1);
  if(data->memory == NULL) {
    free(data);
    return NULL;
  }
  data->size = 0;

  return data;
}

void http_free_data(PHTTP_DATA data) {
  if (data != NULL) {
    if (data->memory != NULL)
      free(data->memory);

    free(data);
  }
}
