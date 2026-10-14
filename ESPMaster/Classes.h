#include <FS.h>

//Streaming FNV-1a for checking completed file writes without keeping their content in RAM.
constexpr uint32_t FILE_CHECKSUM_SEED = 2166136261UL;
inline uint32_t extendFileChecksum(uint32_t checksum, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    checksum = (checksum ^ bytes[i]) * 16777619UL;
  }
  return checksum;
}

//Small write buffer in front of a LittleFS File, so serializeJson() into a file isn't one
//File::write() per character. Counts the bytes the File accepted, so a short write (flash full)
//is detectable.
class BufferedFileWriter : public Print {
public:
  explicit BufferedFileWriter(File &file) : _file(file) {}
  size_t write(uint8_t c) override {
    _checksum = extendFileChecksum(_checksum, &c, 1);
    _buffer[_length++] = c;
    if (_length == sizeof(_buffer)) {
      flush();
    }
    return 1;
  }
  size_t write(const uint8_t *data, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      write(data[i]);
    }
    return size;
  }
  void flush() override {
    if (_length > 0) {
      _accepted += _file.write(_buffer, _length);
      _length = 0;
    }
  }
  size_t accepted() const { return _accepted; }
  uint32_t checksum() const { return _checksum; }
private:
  File &_file;
  uint8_t _buffer[64];
  size_t _length = 0;
  size_t _accepted = 0;
  uint32_t _checksum = FILE_CHECKSUM_SEED;
};

//A message to show at a scheduled future time
struct ScheduledMessage {
  String Message;
  long ScheduledDateTimeUnix;
  bool ShowIndefinitely;
};

//Per-item Alignment/AnimationMode overrides as 1-byte enums instead of 12-byte Strings; Inherit
//means "use the global default". The string conversions live in ServiceRotationFunctions.ino,
//because the ALIGNMENT_MODE_*/ANIMATION_MODE_* constants aren't declared yet at this point.
enum class RotationAlignmentOverride : uint8_t { Inherit, Left, Center, Right };
enum class RotationAnimationModeOverride : uint8_t { Inherit, Simultaneous, LeftToRight, RightToLeft, OutsideIn, InsideOut };

//A rotation item is one of these five kinds (never rotation itself). The global deviceMode stays a
//String - it runs through file I/O, JSON and the boot snapshot, not worth the change for 11 bytes.
//Converted in ServiceRotationFunctions.ino like the enums above.
enum class RotationItemType : uint8_t { Text, Clock, Date, Countdown, Countup };

//One step of a "Rotation" sequence. Type is one of the DEVICE_MODE_* constants (as
//RotationItemType); only the fields that type needs are populated.
struct RotationItem {
  RotationItemType Type = RotationItemType::Text;
  String Value;                //text: the literal text to show
  String Format;                //clock/date: user format string, translated in convertUserDateFormatToEzTime()
  long TargetDateTimeUnix = 0;  //countdown: target date; countup: since-date (same field, meaning depends on Type)
  String Prefix = "";           //countdown/countup: per-item prefix, independent of the standalone mode's setting
  String Suffix = "";           //countdown/countup: per-item suffix, independent of the standalone mode's setting
  int DurationSeconds = 5;      //how long to display this item
  RotationAlignmentOverride Alignment = RotationAlignmentOverride::Inherit; //per-item override; Inherit uses the default alignment
  //AnimationStaggerMs can't be an enum; -1 inherits, since 0 is a valid stagger ("no delay").
  RotationAnimationModeOverride AnimationMode = RotationAnimationModeOverride::Inherit; //per-item override
  int AnimationStaggerMs = -1;     //per-item override; -1 inherits the default stagger
};

//twiboot chip-info response. Lives in a header (not a .ino) so Arduino's auto-generated function
//prototypes can see the type before they're emitted.
struct TwibootChipInfo {
  uint8_t signature[3];
  uint8_t pageSize;
  uint16_t bootloaderStart; //also the max writable flash address for the application
  uint16_t eepromSize;
};

//One configured primary->extension device. In this header for the same reason as TwibootChipInfo:
//pushSegmentToExtension() takes it as a parameter, so the generated prototypes need the type.
struct ExtensionDevice {
  String Host = "";              //hostname or IP; "" = unconfigured slot
  bool Reachable = false;        //last health-check result (ServiceExtensionFunctions.ino)
  int UnitsAmount = 0;           //that device's own unitsAmount, as of the last successful health check
  unsigned long LastCheckedAt = 0;
  //Round trip of the last successful display push - half of it estimates how long the next push
  //takes to arrive, see pushSegmentToExtension()'s startOffsetMs.
  unsigned long LastPushRoundTripMs = 0;
  //Signed-push state (ServiceExtensionFunctions.ino): the extension's current replay nonce, as
  //its last answer named it, and whether its last answer refused the signature (not paired).
  String LinkNonce = "";
  bool LinkRejected = false;
  //Outcome of the last pairing attempt, one of the EXTENSION_PAIR_* values.
  uint8_t PairState = 0;
};
#define EXTENSION_PAIR_NONE 0
#define EXTENSION_PAIR_PENDING 1
#define EXTENSION_PAIR_OK 2
#define EXTENSION_PAIR_WRONG_PASSWORD 3
#define EXTENSION_PAIR_THROTTLED 4
#define EXTENSION_PAIR_FAILED 5

//Workaround for LinkedList conflicting with other libraries.
//From https://github.com/vortigont/LinkedList/commit/39c2628a7794fc6e88e024d7fc82d52bc833c332
namespace LL {
#include <LinkedList.h>
}

template<typename T> using LNode = LL::ListNode<T>;
template<typename T> using LList = LL::LinkedList<T>;
