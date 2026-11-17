// Can a payload reach the cloud model over HTTPS?
//
// Two TLS paths, each against https://openrouter.ai/api/v1/key with no API key (a 401 proves
// the handshake and certificate check worked; nothing secret is sent):
//   A. the ported libcurl + OpenSSL, with the CA bundle linked into the payload. This is the
//      path the host's AgentClient uses, so a pass means it can be reused on the console as-is.
//   B. Sony's own HTTP/2 + TLS stack (sceHttp2) with the system certificates, as a fallback.
// It also prints the console clock (certificate checks fail if it's far off).
// Results go to stdout (the loader's console) and to a notification on the TV.
#include <curl/curl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const char ca_bundle[];
extern const char ca_bundle_end[];

int sceNotificationSend(int userId, bool isLogged, const char* payload);
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);
int sceSslInit(size_t);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2CreateTemplate(int, const char*, int, int);
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
int sceHttp2SendRequest(int, const void*, size_t);
int sceHttp2GetStatusCode(int, int*);

static const char* kUrl = "https://openrouter.ai/api/v1/key";
static const char* kAgent = "ps5-agentic-ui/tls-check";

static void notify(const char* fmt, ...) {
  char msg[256], json[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  for (char* p = msg; *p; ++p)
    if (*p == '"' || *p == '\\') *p = '\'';  // keep the JSON valid
  snprintf(json, sizeof json,
           "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
           "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
           "\"viewData\":{\"message\":{\"body\":\"%s\"},\"subMessage\":{\"body\":\"ps5-agentic-ui tls-check\"}}},"
           "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"expirationDateTime\":\"2036-01-01T00:00:00.000Z\","
           "\"localNotificationId\":\"588194\",\"platformViews\":{},\"viewTemplateType\":\"InteractiveToastTemplateB\"}",
           msg);
  sceNotificationSend(0xFE, true, json);
}

static size_t discard(char* data, size_t size, size_t count, void* user) {
  (void)data;
  (void)user;
  return size * count;
}

/** Path A: libcurl + OpenSSL with the linked-in CA bundle. Returns the HTTP status, or -1. */
static long check_curl(char* detail, size_t detailSize) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    snprintf(detail, detailSize, "curl_easy_init failed");
    return -1;
  }
  struct curl_blob ca = {(void*)ca_bundle, (size_t)(ca_bundle_end - ca_bundle), CURL_BLOB_NOCOPY};
  curl_easy_setopt(curl, CURLOPT_URL, kUrl);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, kAgent);
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
  char err[CURL_ERROR_SIZE] = "";
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
  CURLcode rc = curl_easy_perform(curl);
  long status = -1;
  double total = 0, tls = 0, connect = 0;
  if (rc == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
    snprintf(detail, detailSize, "HTTP %ld, connect %.0f ms, TLS done %.0f ms, total %.0f ms", status, connect * 1000,
             tls * 1000, total * 1000);
  } else {
    snprintf(detail, detailSize, "%s (%s)", curl_easy_strerror(rc), err);
  }
  curl_easy_cleanup(curl);
  return status;
}

#ifdef WITH_SCEHTTP2
/** Path B: Sony's sceHttp2 with the system certificates. Returns the HTTP status, or -1. */
static int check_sce(char* detail, size_t detailSize) {
  int pool, ssl, http, tmpl, req, status = -1;
  sceNetInit();  // fails harmlessly if already initialised
  if ((pool = sceNetPoolCreate("tls-check", 32 * 1024, 0)) < 0) return snprintf(detail, detailSize, "sceNetPoolCreate 0x%x", pool), -1;
  if ((ssl = sceSslInit(256 * 1024)) < 0) return snprintf(detail, detailSize, "sceSslInit 0x%x", ssl), -1;
  if ((http = sceHttp2Init(pool, ssl, 256 * 1024, 1)) < 0) return snprintf(detail, detailSize, "sceHttp2Init 0x%x", http), -1;
  if ((tmpl = sceHttp2CreateTemplate(http, kAgent, 3, 1)) < 0) return snprintf(detail, detailSize, "CreateTemplate 0x%x", tmpl), -1;
  if ((req = sceHttp2CreateRequestWithURL(tmpl, "GET", kUrl, 0)) < 0) return snprintf(detail, detailSize, "CreateRequest 0x%x", req), -1;
  int rc = sceHttp2SendRequest(req, NULL, 0);
  if (rc < 0) return snprintf(detail, detailSize, "SendRequest 0x%x", rc), -1;
  if ((rc = sceHttp2GetStatusCode(req, &status)) < 0) return snprintf(detail, detailSize, "GetStatusCode 0x%x", rc), -1;
  snprintf(detail, detailSize, "HTTP %d", status);
  return status;
}
#endif

int main(void) {
  time_t now = time(NULL);
  char when[64];
  strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", gmtime(&now));
  printf("[tls-check] console clock: %s\n", when);
  printf("[tls-check] %s\n", curl_version());
  printf("[tls-check] CA bundle: %zu bytes\n", (size_t)(ca_bundle_end - ca_bundle));

  curl_global_init(CURL_GLOBAL_DEFAULT);
  char a[256], b[256];
  long sa = check_curl(a, sizeof a);
  printf("[tls-check] A libcurl+OpenSSL: %s\n", a);
#ifdef WITH_SCEHTTP2
  int sb = check_sce(b, sizeof b);
  printf("[tls-check] B sceHttp2: %s\n", b);
#else
  // Run 1 (4 Nov 2026): sceHttp2 failed with 0x8095f00c and the payload aborted at exit
  // ("resource deadlock would occur"). Off by default; build with make SCEHTTP2=1 to try it.
  int sb = -1;
  snprintf(b, sizeof b, "skipped");
#endif
  curl_global_cleanup();

  // 401 (no key sent) means the request reached the provider over verified TLS.
  bool okA = sa == 401 || sa == 200, okB = sb == 401 || sb == 200;
  notify("TLS to the model provider: curl %s, sceHttp2 %s", okA ? "OK" : "FAILED", okB ? "OK" : b);
  printf("[tls-check] result: curl %s, sceHttp2 %s\n", okA ? "OK" : "FAILED", okB ? "OK" : b);
  // Runs 1 and 2 aborted after main returned ("resource deadlock would occur" from the exit
  // path). End the process without running exit handlers; see docs/findings/07-m2-tls.md.
  printf("[tls-check] exiting with _exit\n");
  fflush(stdout);
  _exit(okA || okB ? 0 : 1);
}
