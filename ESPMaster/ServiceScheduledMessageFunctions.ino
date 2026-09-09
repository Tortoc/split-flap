//Replace/add a scheduled message and persist the result afterwards
void addAndPersistScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  addScheduledMessage(scheduledText, scheduledDateTimeUnix, showIndefinitely);
  writeScheduledMessagesToFile();
}

//Replace/add a scheduled message
void addScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  SerialPrintln(F("Processing New Scheduled Message"));
  SerialPrintln(F("-- Message: ") + scheduledText);
  SerialPrint(F("-- Unix: "));
  SerialPrintln(scheduledDateTimeUnix);
  SerialPrint(F("-- Show Indefinitely: "));
  SerialPrintln(showIndefinitely ? F("Yes") : F("No"));

  //Find the existing scheduled message and update it if one exists
  //.get() (not operator[]) - the library's operator[] dereferences a NULL node with no bounds
  //check on a stale/invalid index; .get() returns a default-constructed value instead. See
  //BUGFIX_LOG #117 - this loop itself never yields mid-iteration so isn't the reproduced hang,
  //but kept consistent with the other scheduledMessages accessors as a precaution.
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

    if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln(F("Removing Existing Scheduled Message due to be shown, it will be replaced"));
      scheduledMessages.remove(scheduledMessageIndex);
      break;
    }
  }

  //Add the new scheduled message with the new value
  //Bug (found while fixing #117): timezone.now() doesn't return real UTC despite its name/params
  //(verified empirically - see getRealUtcNow()'s comment in ESPMaster.ino), but scheduledDateTimeUnix
  //arrives from the client as real UTC seconds - comparing them directly was off by the timezone
  //offset (2h for Europe/Berlin in summer), rejecting messages scheduled less than that far ahead
  //as "already in the past".
  if (scheduledDateTimeUnix > getRealUtcNow()) {
    SerialPrintln(F("Adding new Scheduled Message"));
    scheduledMessages.add({scheduledText, scheduledDateTimeUnix, showIndefinitely});
  }
  else {
    SerialPrintln(F("Not adding Scheduled Message as it is in the past"));
  }
}

//Remove and delete a existing scheduled message if found
bool removeScheduledMessage(long scheduledDateTimeUnix) {
  //Find the existing scheduled message and delete it
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

    if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln(F("Deleting Scheduled Message due to be shown: ") + scheduledMessage.Message);
      scheduledMessages.remove(scheduledMessageIndex);
      
      writeScheduledMessagesToFile();

      return true;
    }
  }
  
  return false;
}

//Show and remove any scheduled message whose time has passed
void checkScheduledMessages() {
  //Defer rather than silently consume a due message while parked for the night - showText()
  //would blank it anyway, but removing it from the queue here would lose it for good instead of
  //it firing for real once Quiet Hours end.
  if (isQuietHoursActive()) {
    return;
  }

  //See the matching comment in addScheduledMessage() above - must compare against real UTC, not
  //timezone.now()'s (misleadingly-named) local time, to match the UTC seconds the client
  //scheduled against.
  unsigned long currentTimeUnix = getRealUtcNow();
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

    if (currentTimeUnix > scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln(F("Scheduled Message due to be shown: ") + scheduledMessage.Message);
      SerialPrint(F("Scheduled Message to be Shown Indefinitely: "));
      SerialPrintln(scheduledMessage.ShowIndefinitely ? F("Yes") : F("No"));

      bool shown;
      if (scheduledMessage.ShowIndefinitely) {
        deviceMode = DEVICE_MODE_TEXT;
        inputText = scheduledMessage.Message;
        shown = showText(scheduledMessage.Message);
      }
      else {
        shown = showText(scheduledMessage.Message, scheduledMessageDisplayTimeMillis);
      }

      //Only dequeue on actual success (found 2026-09-09, CODE_REVIEW.md #10) - a busy display or
      //no detected units previously still removed the entry unconditionally, silently losing the
      //message. Leaving it queued means the next ~1s tick retries the same (still-due) message;
      //no risk of it firing twice, since ShowIndefinitely's own deviceMode/inputText assignment
      //above already makes the very next tick's normal mode-dispatch show it regardless of this
      //call's own result, and a still-queued entry just means this function tries again too.
      if (shown) {
        scheduledMessages.remove(scheduledMessageIndex);
        writeScheduledMessagesToFile();
      }
      break;
    }
  }
}

//Parse JSON scheduled messages into the current known scheduled messages
void readScheduledMessagesFromJson(String scheduledMessagesJson) {
  if (scheduledMessagesJson != "") {    
    int addedScheduledMessageCount = 0;

    JsonDocument jsonDocument;
    DeserializationError deserialisationError = deserializeJson(jsonDocument, scheduledMessagesJson);

    if (!deserialisationError) {
      if (jsonDocument.is<JsonArray>()) {
        for (JsonVariant value : jsonDocument.as<JsonArray>()) {
          long scheduledDateTimeUnix = value["scheduledDateTimeUnix"];
          String message = value["message"];
          bool showIndefinitely = value["showIndefinitely"];

          addScheduledMessage(message, scheduledDateTimeUnix, showIndefinitely);
          addedScheduledMessageCount++;
        }
        
        //If there is a difference in scheduled messages, re-write the messages
        if (addedScheduledMessageCount != scheduledMessages.size()) {
          SerialPrintln(F("Read message count and scheduled message count differ, writing updated messages"));
          writeScheduledMessagesToFile();
        }
      }
      else {
        SerialPrintln(F("Invalid JSON Array found, scrapping and starting again"));
        writeEmptyScheduledMessagesToFile();
      }
    }
    else {
      SerialPrintln(F("Invalid JSON found, scrapping and starting again"));
      writeEmptyScheduledMessagesToFile();
    }
  }
}

//Convert the scheduled messages to a JSON document and save to file
void writeScheduledMessagesToFile() {
  if (scheduledMessages.size()) {
    JsonDocument scheduledMessagesDocument;
    for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
      ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

      scheduledMessagesDocument[scheduledMessageIndex]["scheduledDateTimeUnix"] = scheduledMessage.ScheduledDateTimeUnix;
      scheduledMessagesDocument[scheduledMessageIndex]["message"] = scheduledMessage.Message;
      scheduledMessagesDocument[scheduledMessageIndex]["showIndefinitely"] = scheduledMessage.ShowIndefinitely;
    }

    String scheduledMessagesJson;
    serializeJson(scheduledMessagesDocument, scheduledMessagesJson);
    writeFile(LittleFS, scheduledMessagesPath, scheduledMessagesJson.c_str());
  }
  else {
    SerialPrintln(F("No Scheduled Messages Left - Writing Empty to File"));
    writeEmptyScheduledMessagesToFile();
  }
}

//Write an empty array to the schedule messages file, used in case something goes terribly wrong
void writeEmptyScheduledMessagesToFile() {
  writeFile(LittleFS, scheduledMessagesPath, "[]");
}
