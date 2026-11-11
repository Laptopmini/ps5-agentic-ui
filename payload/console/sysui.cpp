#include "sysui.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// Layouts as on the PS4 (Sony's common dialog headers); a PS5 emulator (AnyPS5) reads games'
// message dialog parameters at the same offsets (mode at 0x38, user message at 0x40).
namespace {

struct CommonDialogBaseParam {
  size_t size;
  uint8_t reserved[36];
  uint32_t magic;
} __attribute__((aligned(8)));

struct ButtonsParam {
  const char* msg1;
  const char* msg2;
  char reserved[32];
};

struct UserMessageParam {
  int32_t buttonType;
  int32_t pad;
  const char* msg;
  ButtonsParam* buttonsParam;
  char reserved[24];
};

struct MsgDialogParam {
  CommonDialogBaseParam baseParam;
  size_t size;
  int32_t mode;
  int32_t pad;
  UserMessageParam* userMsgParam;
  void* progBarParam;
  void* sysMsgParam;
  int32_t userId;
  char reserved[40];
  int32_t pad2;
};
static_assert(offsetof(MsgDialogParam, mode) == 0x38 && offsetof(MsgDialogParam, userMsgParam) == 0x40);

struct ProgressBarParam {
  int32_t barType;  // 0: percentage
  int32_t pad;
  const char* msg;
  char reserved[64];
};

struct MsgDialogResult {
  int32_t mode;
  int32_t result;
  int32_t buttonId;
  char reserved[32];
};

constexpr uint32_t kMagic = 0xC0D1A109;
constexpr int32_t kModeUserMsg = 1, kModeProgressBar = 2;
constexpr int32_t kButtonsYesNo = 1, kButtonsTwo = 9;  // 2BUTTONS takes our own labels
constexpr int kStatusFinished = 3, kStatusRunning = 2;
constexpr int32_t kButton1 = 1, kButton2 = 2;

int (*CommonDialogInitialize)();
int (*MsgDialogInitialize)();
int (*MsgDialogOpen)(const MsgDialogParam*);
int (*MsgDialogUpdateStatus)();
int (*MsgDialogGetResult)(MsgDialogResult*);
int (*MsgDialogTerminate)();
int (*MsgDialogClose)();
int (*ProgressBarSetValue)(int target, uint32_t rate);

bool g_ready = false, g_open = false, g_progress = false;
ProgressBarParam g_bar;
uint32_t g_rate = 0;
bool g_cancelFirst = false;  // which button is "cancel" in the open dialog
int32_t g_buttons = 0;
std::string g_message, g_label1, g_label2;
ButtonsParam g_buttonsParam;
UserMessageParam g_user;
MsgDialogParam g_param;

}  // namespace

extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(int*);
int sceNotificationSend(int userId, bool isLogged, const char* payload);
int sceSystemServiceGetStatus(void* status);
int sceSystemServiceReceiveEvent(void* event);
}

namespace {
// Layouts as the PS4 SDK has them (shadPS4's systemservice.h); padded in case the PS5's are larger.
struct SystemServiceStatus {
  int32_t eventNum;
  bool isSystemUiOverlaid, isInBackgroundExecution, isCpuMode7CpuNormal, isGameLiveStreamingOnAir, isOutOfVrPlayArea;
  uint8_t reserved[256];
};
struct SystemServiceEvent {
  uint32_t eventType;
  char param[8192 + 256];
};
}  // namespace

namespace sysui {

bool init(std::string& why) {
  void* common = dlopen("libSceCommonDialog.sprx", RTLD_LAZY);
  void* msg = dlopen("libSceMsgDialog.sprx", RTLD_LAZY);
  if (!common || !msg) {
    why = std::string("dlopen: ") + (dlerror() ? dlerror() : "?");
    return false;
  }
  CommonDialogInitialize = (int (*)())dlsym(common, "sceCommonDialogInitialize");
  MsgDialogInitialize = (int (*)())dlsym(msg, "sceMsgDialogInitialize");
  MsgDialogOpen = (int (*)(const MsgDialogParam*))dlsym(msg, "sceMsgDialogOpen");
  MsgDialogUpdateStatus = (int (*)())dlsym(msg, "sceMsgDialogUpdateStatus");
  MsgDialogGetResult = (int (*)(MsgDialogResult*))dlsym(msg, "sceMsgDialogGetResult");
  MsgDialogTerminate = (int (*)())dlsym(msg, "sceMsgDialogTerminate");
  MsgDialogClose = (int (*)())dlsym(msg, "sceMsgDialogClose");
  ProgressBarSetValue = (int (*)(int, uint32_t))dlsym(msg, "sceMsgDialogProgressBarSetValue");
  if (!CommonDialogInitialize || !MsgDialogInitialize || !MsgDialogOpen || !MsgDialogUpdateStatus || !MsgDialogGetResult || !MsgDialogTerminate || !MsgDialogClose) {
    why = "missing symbols in libSceMsgDialog";
    return false;
  }
  int err = CommonDialogInitialize();  // may already be up (the keyboard uses it): fine
  char buf[64];
  std::snprintf(buf, sizeof buf, "sceCommonDialogInitialize 0x%08x", unsigned(err));
  why = buf;
  g_ready = true;
  return true;
}

static int openWith(int32_t buttons) {
  int user = 0;
  sceUserServiceInitialize(nullptr);
  sceUserServiceGetForegroundUser(&user);
  MsgDialogInitialize();  // per dialog, terminated after the answer

  std::memset(&g_param, 0, sizeof g_param);
  std::memset(&g_user, 0, sizeof g_user);
  std::memset(&g_buttonsParam, 0, sizeof g_buttonsParam);
  g_param.baseParam.size = sizeof g_param.baseParam;
  g_param.baseParam.magic = kMagic + uint32_t(uintptr_t(&g_param.baseParam));
  g_param.size = sizeof g_param;
  g_param.mode = kModeUserMsg;
  g_param.userMsgParam = &g_user;
  g_param.userId = user;
  g_user.buttonType = buttons;
  g_user.msg = g_message.c_str();
  if (buttons == kButtonsTwo) {
    g_buttonsParam.msg1 = g_label1.c_str();
    g_buttonsParam.msg2 = g_label2.c_str();
    g_user.buttonsParam = &g_buttonsParam;
  }
  int err = MsgDialogOpen(&g_param);
  if (err) MsgDialogTerminate();
  return err;
}

int openConfirm(const Confirm& c, std::string& how) {
  if (!g_ready) return -1;
  if (g_progress) closeProgress();
  if (g_open) close();
  g_message = c.message;
  // Destructive: the safe choice comes first, so it has the initial focus (as on the console).
  g_cancelFirst = c.destructive;
  g_label1 = g_cancelFirst ? c.cancelLabel : c.confirmLabel;
  g_label2 = g_cancelFirst ? c.confirmLabel : c.cancelLabel;
  int err = openWith(kButtonsTwo);
  char buf[96];
  if (err == 0) {
    g_buttons = kButtonsTwo;
    how = "two buttons \"" + g_label1 + "\" / \"" + g_label2 + "\"";
  } else {
    // Fall back to the plain Yes/No dialog (Yes = confirm).
    std::snprintf(buf, sizeof buf, "two buttons failed 0x%08x; ", unsigned(err));
    how = buf;
    err = openWith(kButtonsYesNo);
    g_buttons = kButtonsYesNo;
    g_cancelFirst = false;
    how += err ? "yes/no failed too" : "yes/no";
  }
  g_open = err == 0;
  return err;
}

State poll() {
  if (!g_open || g_progress) return State::Idle;
  int status = MsgDialogUpdateStatus();
  if (status == kStatusRunning || status < kStatusFinished) return State::Open;
  MsgDialogResult r{};
  MsgDialogGetResult(&r);
  MsgDialogTerminate();
  g_open = false;
  // result 0 = a button was pressed; anything else (Circle, closed) is a cancel.
  if (r.result != 0) return State::Cancelled;
  bool first = r.buttonId == kButton1;
  if (g_buttons == kButtonsYesNo) return first ? State::Confirmed : State::Cancelled;  // Yes = 1
  bool confirmed = g_cancelFirst ? r.buttonId == kButton2 : first;
  return confirmed ? State::Confirmed : State::Cancelled;
}

void close() {
  if (!g_open) return;
  g_progress = false;
  MsgDialogClose();
  MsgDialogTerminate();
  g_open = false;
}

int openProgress(const std::string& label) {
  if (!g_ready) return -1;
  if (g_open) close();
  int user = 0;
  sceUserServiceInitialize(nullptr);
  sceUserServiceGetForegroundUser(&user);
  MsgDialogInitialize();
  g_message = label;
  std::memset(&g_param, 0, sizeof g_param);
  std::memset(&g_bar, 0, sizeof g_bar);
  g_param.baseParam.size = sizeof g_param.baseParam;
  g_param.baseParam.magic = kMagic + uint32_t(uintptr_t(&g_param.baseParam));
  g_param.size = sizeof g_param;
  g_param.mode = kModeProgressBar;
  g_param.progBarParam = &g_bar;
  g_param.userId = user;
  g_bar.barType = 0;
  g_bar.msg = g_message.c_str();
  int err = MsgDialogOpen(&g_param);
  if (err) {
    MsgDialogTerminate();
    return err;
  }
  g_open = g_progress = true;
  g_rate = 0;
  return 0;
}

void setProgress(float value) {
  if (!g_progress || !ProgressBarSetValue) return;
  MsgDialogUpdateStatus();  // every frame while the dialog is up
  uint32_t rate = uint32_t(value < 0 ? 0 : value > 1 ? 100 : value * 100 + 0.5f);
  if (rate == g_rate) return;
  g_rate = rate;
  ProgressBarSetValue(0, rate);
}

bool progressOpen() { return g_progress; }

void closeProgress() {
  if (!g_progress) return;
  MsgDialogClose();
  MsgDialogTerminate();
  g_open = g_progress = false;
}

void notify(const std::string& text, const std::string& icon) {
  std::string body = text;
  for (char& ch : body)
    if (ch == '"' || ch == '\\' || ch == '\n') ch = ch == '\n' ? ' ' : '\'';  // keep the JSON valid
  (void)icon;
  char json[1400];
  std::snprintf(json, sizeof json,
                "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
                "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
                "\"viewData\":{\"message\":{\"body\":\"%.900s\"},\"subMessage\":{\"body\":\"PS5 A2UI\"}}},"
                "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"expirationDateTime\":\"2036-01-01T00:00:00.000Z\","
                "\"localNotificationId\":\"588195\",\"platformViews\":{},\"viewTemplateType\":\"InteractiveToastTemplateB\"}",
                body.c_str());
  sceNotificationSend(0xFE, true, json);
}

SystemState pollSystem() {
  SystemState out;
  SystemServiceStatus st{};
  out.status = sceSystemServiceGetStatus(&st);
  if (out.status != 0) return out;
  out.overlaid = st.isSystemUiOverlaid;
  out.background = st.isInBackgroundExecution;
  for (int i = 0; i < st.eventNum && i < 8; ++i) {
    static SystemServiceEvent e;  // 8 KB: not on the stack
    if (sceSystemServiceReceiveEvent(&e) != 0) break;
    out.events.push_back(e.eventType);
  }
  return out;
}

}  // namespace sysui
