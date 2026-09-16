//Replace/add a scheduled message and persist the result afterwards - returns false (and skips the
//write) if rejected as already due/in the past.
bool addAndPersistScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  bool added = addScheduledMessage(scheduledText, scheduledDateTimeUnix, showIndefinitely);
  if (added) {
    writeScheduledMessagesToFile();
  }
  return added;
}

//Replace/add a scheduled message. Returns whether it was actually added, so the caller can report
//failure for an already-past-due timestamp instead of always answering 200 OK.
bool addScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  SerialPrintln(F("Processing New Scheduled Message"));
  SerialPrintln(F("-- Message: ") + scheduledText);
  SerialPrint(F("-- Unix: "));
  SerialPrintln(scheduledDateTimeUnix);
  SerialPrint(F("-- Show Indefinitely: "));
  SerialPrintln(showIndefinitely ? F("Yes") : F("No"));

  //Reject a past-due timestamp FIRST, before touching the existing list at all - otherwise a
  //resubmission matching an existing stored timestamp could delete that still-valid entry with
  //nothing to replace it. timezone.now() doesn't return real UTC - see getRealUtcNow()'s comment.
  if (scheduledDateTimeUnix > getRealUtcNow()) {
    //Find the existing scheduled message and replace it if one exists.
    //.get() (not operator[]) - the library's operator[] dereferences a NULL node with no bounds
    //check on a stale/invalid index; .get() returns a default-constructed value instead. This
    //loop itself never yields mid-iteration so isn't a reproduced hang site, but kept consistent
    //with the other scheduledMessages accessors as a precaution.
    bool replacedExisting = false;
    for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
      ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

      if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
        SerialPrintln(F("Removing Existing Scheduled Message due to be shown, it will be replaced"));
        scheduledMessages.remove(scheduledMessageIndex);
        replacedExisting = true;
        break;
      }
    }

    //Only a genuinely NEW entry needs to be checked against the cap.
    if (!replacedExisting && scheduledMessages.size() >= SCHEDULED_MESSAGES_MAX) {
      SerialPrintln(F("Not adding Scheduled Message - already at SCHEDULED_MESSAGES_MAX"));
      return false;
    }

    SerialPrintln(F("Adding new Scheduled Message"));
    scheduledMessages.add({scheduledText, scheduledDateTimeUnix, showIndefinitely});
    return true;
  }
  else {
    SerialPrintln(F("Not adding Scheduled Message as it is in the past"));
    return false;
  }
}

//Restores one PERSISTED scheduled message at boot, unconditionally - unlike addScheduledMessage()
//above, this does NOT reject a timestamp that's already due/in the past; checkScheduledMessages()
//picks up anything genuinely due on its own next tick. No replace-if-same-timestamp logic needed
//here either - a freshly-loaded list can't contain a duplicate timestamp.
void restoreScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  scheduledMessages.add({scheduledText, scheduledDateTimeUnix, showIndefinitely});
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

  unsigned long currentTimeUnix = getRealUtcNow();
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

    if (currentTimeUnix > scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln(F("Scheduled Message due to be shown: ") + scheduledMessage.Message);
      SerialPrint(F("Scheduled Message to be Shown Indefinitely: "));
      SerialPrintln(scheduledMessage.ShowIndefinitely ? F("Yes") : F("No"));

      //A message that needs more lines than fit can NEVER be shown - unlike a busy display
      //(transient), leaving this queued would retry the same failing call forever and permanently
      //block every OTHER due message behind it. Drop it here instead.
      if (isMessageTooLongToRender(scheduledMessage.Message)) {
        SerialPrintln(F("Scheduled Message needs more lines than fit and can never be shown - dropping it instead of blocking later messages"));
        scheduledMessages.remove(scheduledMessageIndex);
        writeScheduledMessagesToFile();
        break;
      }

      bool shown;
      if (scheduledMessage.ShowIndefinitely) {
        deviceMode = DEVICE_MODE_TEXT;
        inputText = scheduledMessage.Message;
        shown = showText(scheduledMessage.Message);
      }
      else {
        shown = showText(scheduledMessage.Message, scheduledMessageDisplayTimeMillis);
      }

      //Only dequeue on actual success - a busy display or no detected units otherwise removes the
      //entry unconditionally, silently losing the message.
      if (shown) {
        scheduledMessages.remove(scheduledMessageIndex);
        writeScheduledMessagesToFile();
      }
      break;
    }
  }
}

//Parse JSON scheduled messages into the current known scheduled messages - called once at boot
//(loadValuesFromFileSystem()), restoring whatever was persisted from the last session.
void readScheduledMessagesFromJson(String scheduledMessagesJson) {
  if (scheduledMessagesJson != "") {
    JsonDocument jsonDocument;
    DeserializationError deserialisationError = deserializeJson(jsonDocument, scheduledMessagesJson);

    if (!deserialisationError) {
      if (jsonDocument.is<JsonArray>()) {
        for (JsonVariant value : jsonDocument.as<JsonArray>()) {
          //Only reachable if the persisted file itself somehow holds more than the cap -
          //addScheduledMessage() already enforces it for every new submission.
          if (scheduledMessages.size() >= SCHEDULED_MESSAGES_MAX) {
            SerialPrintln(F("Scheduled messages file has more than SCHEDULED_MESSAGES_MAX entries - dropping the rest"));
            break;
          }

          long scheduledDateTimeUnix = value["scheduledDateTimeUnix"];
          String message = value["message"];
          bool showIndefinitely = value["showIndefinitely"];

          //restoreScheduledMessage(), not addScheduledMessage() - a restored entry must never be
          //rejected just because it's already due; see that function's own comment.
          restoreScheduledMessage(message, scheduledDateTimeUnix, showIndefinitely);
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
