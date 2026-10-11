#include <FS.h>

//Small write buffer in front of a LittleFS File, so serializeJson() streaming straight into a file
//doesn't turn into one File::write() call per JSON punctuation character (CODE_REVIEW_V7.md #1).
//LittleFS caches/programs whole pages either way - this only saves call overhead, not flash wear.
//Counts the bytes the File actually accepted, so a short write (flash full) is detectable.
class BufferedFileWriter : public Print {
public:
  explicit BufferedFileWriter(File &file) : _file(file) {}
  size_t write(uint8_t c) override {
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
private:
  File &_file;
  uint8_t _buffer[64];
  size_t _length = 0;
  size_t _accepted = 0;
};

//A message to show at a scheduled future time
struct ScheduledMessage {
  String Message;
  long ScheduledDateTimeUnix;
  bool ShowIndefinitely;
};

//Compact representations of RotationItem's Alignment/AnimationMode per-item overrides (found
//2026-09-12, RAM-savings review): each only ever holds one of a small closed set of values - a
//String costs 12 bytes per slot regardless of content, an enum backed by uint8_t costs 1. Inherit
//(0) is the previous ""-sentinel ("use the global default"), preserved exactly. Conversion to/from
//the wire/persisted string form (rotationAlignmentOverrideToString()/
//stringToRotationAlignmentOverride() and their AnimationMode equivalents) lives in
//ServiceRotationFunctions.ino, right next to the ALIGNMENT_MODE_*/ANIMATION_MODE_* string
//constants they translate against (ESPMaster.ino) - deliberately NOT here, since those constants
//aren't declared yet at the point this header is included (see ESPMaster.ino's own include-order
//comment on isReservedI2CAddress() for the general version of this gotcha).
enum class RotationAlignmentOverride : uint8_t { Inherit, Left, Center, Right };
enum class RotationAnimationModeOverride : uint8_t { Inherit, Simultaneous, LeftToRight, RightToLeft, OutsideIn, InsideOut };

//Same reasoning for RotationItem's own Type field (found 2026-09-12, same review, done as a
//second pass after Alignment/AnimationMode above): a rotation item can only ever be one of these
//5 kinds (never "rotation" itself - that would be self-referential, and DEVICE_MODE_ROTATION is
//deliberately not a value here). This is NARROWER in scope than converting the global `deviceMode`
//String itself, which shares these same DEVICE_MODE_* constants but is threaded through file I/O,
//JSON (de)serialization and the boot-snapshot signature string all over ESPMaster.ino/
//ServiceFileSystemFunctions.ino - left as String, not worth that much larger blast radius for 11
//bytes of savings on a single global. rotationItemTypeToString()/stringToRotationItemType()
//(ServiceRotationFunctions.ino) convert at the same JSON boundaries as the enums above.
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
  //Same "override, Inherit uses the default" pattern as Alignment above - found 2026-09-10 on user
  //request ("Animation pro Rotation-Item"). AnimationStaggerMs can't use the same enum trick (it's
  //a number, not a closed set) - -1 is used instead since 0 is itself a real, valid stagger value
  //(explicitly "no delay" rather than "inherit").
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

//One configured Primary->Extension device (multiple-units.md, Phase 1) - same "lives in a header"
//reasoning as TwibootChipInfo above: ServiceExtensionFunctions.ino's pushSegmentToExtension() takes
//this as a function parameter, so it must be visible before Arduino's auto-generated prototype
//block (emitted near the top of the concatenated sketch) references it - declaring it later in
//ESPMaster.ino, alongside the actual extensionDevices[] array it's used for, broke the build the
//same way TwibootChipInfo once did (found 2026-09-15, verified by the actual compiler error, not
//assumed from precedent alone).
struct ExtensionDevice {
  String Host = "";              //hostname or IP; "" = unconfigured slot
  bool Reachable = false;        //last health-check result (ServiceExtensionFunctions.ino)
  int UnitsAmount = 0;           //that device's own unitsAmount, as of the last successful health check
  unsigned long LastCheckedAt = 0;
  //Round trip of the last successful display push - half of it estimates how long the next push
  //takes to arrive, see pushSegmentToExtension()'s startOffsetMs.
  unsigned long LastPushRoundTripMs = 0;
};

//Workaround for LinkedList conflicting with other libraries.
//From https://github.com/vortigont/LinkedList/commit/39c2628a7794fc6e88e024d7fc82d52bc833c332
namespace LL {
#include <LinkedList.h>
}

template<typename T> using LNode = LL::ListNode<T>;
template<typename T> using LList = LL::LinkedList<T>;
