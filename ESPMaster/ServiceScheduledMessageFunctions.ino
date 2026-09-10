//Replace/add a scheduled message and persist the result afterwards - returns false (and skips the
//write, nothing changed) if the message was rejected as already due/in the past, same reasoning
//as addScheduledMessage() below (found 2026-09-10, CODE_REVIEW_V4.md #12).
bool addAndPersistScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  bool added = addScheduledMessage(scheduledText, scheduledDateTimeUnix, showIndefinitely);
  if (added) {
    writeScheduledMessagesToFile();
  }
  return added;
}

//Replace/add a scheduled message. Returns whether it was actually added - found 2026-09-10,
//CODE_REVIEW_V4.md #12: this used to be void, silently no-op'ing (just a serial log line) for a
//past-due timestamp while its caller (POST /api/settings' handler) had no way to know and always
//answered 200 "OK" regardless - a schedule request with a timestamp that's already passed (or
//passes before this runs) looked successful to the client but nothing was actually scheduled.
bool addScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  SerialPrintln(F("Processing New Scheduled Message"));
  SerialPrintln(F("-- Message: ") + scheduledText);
  SerialPrint(F("-- Unix: "));
  SerialPrintln(scheduledDateTimeUnix);
  SerialPrint(F("-- Show Indefinitely: "));
  SerialPrintln(showIndefinitely ? F("Yes") : F("No"));

  //Reject a past-due timestamp FIRST, before touching the existing list at all - found 2026-09-10
  //while implementing CODE_REVIEW_V4.md #12 (not itself part of that review): the old order
  //removed any existing entry sharing this exact timestamp, THEN checked whether the new one was
  //even in the future. A resubmission of an entry that's already essentially due (matching an
  //existing stored timestamp) but rejected here as past-due would otherwise delete that still-
  //valid existing entry with nothing to replace it with - a narrow window (the existing entry
  //would itself already have to be right on the verge of firing, since checkScheduledMessages()
  //fires/removes genuinely due entries on its own every second), but a real, avoidable loss.
  //Bug (found while fixing #117): timezone.now() doesn't return real UTC despite its name/params
  //(verified empirically - see getRealUtcNow()'s comment in ESPMaster.ino), but scheduledDateTimeUnix
  //arrives from the client as real UTC seconds - comparing them directly was off by the timezone
  //offset (2h for Europe/Berlin in summer), rejecting messages scheduled less than that far ahead
  //as "already in the past".
  if (scheduledDateTimeUnix > getRealUtcNow()) {
    //Find the existing scheduled message and replace it if one exists.
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

    SerialPrintln(F("Adding new Scheduled Message"));
    scheduledMessages.add({scheduledText, scheduledDateTimeUnix, showIndefinitely});
    return true;
  }
  else {
    SerialPrintln(F("Not adding Scheduled Message as it is in the past"));
    return false;
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

      //A message that needs more lines than fit can NEVER be shown - unlike a busy display
      //(transient, resolves on its own), leaving this queued would retry the exact same failing
      //showText() call every tick forever, and the break below would then permanently block every
      //OTHER due message queued behind it from ever being reached (found 2026-09-09,
      //CODE_REVIEW_V2.md #10). New submissions are now rejected up front (see the /api/settings
      //and /api/text handlers), but this stays as defense in depth for anything scheduled before
      //that validation existed. Drop it here instead.
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
