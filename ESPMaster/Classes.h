//Used to temporarily store data to show at a later time/date
struct ScheduledMessage {
  String Message;
  long ScheduledDateTimeUnix;
  bool ShowIndefinitely;
};

//twiboot chip-info response (see ServiceUnitFirmwareFunctions.ino). Must live in a real header,
//not a .ino file: Arduino's auto-generated function prototypes are inserted above everything in
//the concatenated sketch, so a struct used as a function parameter type has to already be defined
//by the time those prototypes are emitted - a #include like this one is processed first, a struct
//defined further down in another .ino file is not.
struct TwibootChipInfo {
  uint8_t signature[3];
  uint8_t pageSize;
  uint16_t bootloaderStart; //also the max writable flash address for the application
  uint16_t eepromSize;
};

//Used as a work around for issues that linked list has with other libraries
//Taken from here: https://github.com/vortigont/LinkedList/commit/39c2628a7794fc6e88e024d7fc82d52bc833c332
//Credit: vortigont
namespace LL { 
#include <LinkedList.h>
}

template<typename T> using LNode = LL::ListNode<T>;
template<typename T> using LList = LL::LinkedList<T>;
