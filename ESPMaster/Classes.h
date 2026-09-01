//A message to show at a scheduled future time
struct ScheduledMessage {
  String Message;
  long ScheduledDateTimeUnix;
  bool ShowIndefinitely;
};

//One step of a "Rotation" sequence. Type is one of the DEVICE_MODE_* constants; only the fields
//that type needs are populated.
struct RotationItem {
  String Type;
  String Value;                //text: the literal text to show
  String Format;                //clock/date: user format string, translated in convertUserDateFormatToEzTime()
  long TargetDateTimeUnix = 0;  //countdown: target date; countup: since-date (same field, meaning depends on Type)
  String Prefix = "";           //countdown/countup: per-item prefix, independent of the standalone mode's setting
  String Suffix = "";           //countdown/countup: per-item suffix, independent of the standalone mode's setting
  int DurationSeconds = 5;      //how long to display this item
  String Alignment = "";        //per-item override; "" inherits the default alignment
};

//twiboot chip-info response. Lives in a header (not a .ino) so Arduino's auto-generated function
//prototypes can see the type before they're emitted.
struct TwibootChipInfo {
  uint8_t signature[3];
  uint8_t pageSize;
  uint16_t bootloaderStart; //also the max writable flash address for the application
  uint16_t eepromSize;
};

//Workaround for LinkedList conflicting with other libraries.
//From https://github.com/vortigont/LinkedList/commit/39c2628a7794fc6e88e024d7fc82d52bc833c332
namespace LL {
#include <LinkedList.h>
}

template<typename T> using LNode = LL::ListNode<T>;
template<typename T> using LList = LL::LinkedList<T>;
