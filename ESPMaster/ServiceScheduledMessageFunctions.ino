//Replace/add a scheduled message and persist the result afterwards. SCHEDULE_SAVE_FAILED: the
//list couldn't be saved (e.g. out of memory) - it's reloaded from the untouched file, so memory
//and flash agree again and the message counts as not scheduled (CODE_REVIEW_V7.md #1).
int addAndPersistScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  if (!ensureScheduledMessagesComplete()) {
    return SCHEDULE_SAVE_FAILED;
  }
  //An entry with the same timestamp gets replaced by addScheduledMessage(), so its text doesn't count.
  if (scheduledMessagesTextLength(scheduledDateTimeUnix) + scheduledText.length() > STORED_TEXT_BUDGET) {
    return SCHEDULE_OVER_BUDGET;
  }
  if (!addScheduledMessage(scheduledText, scheduledDateTimeUnix, showIndefinitely)) {
    return SCHEDULE_REJECTED;
  }
  if (!writeScheduledMessagesToFile()) {
    reloadScheduledMessagesFromFile();
    return SCHEDULE_SAVE_FAILED;
  }
  return SCHEDULE_ADDED;
}

//Total length of all stored scheduled message texts, leaving out the one at excludeTimestamp - see
//STORED_TEXT_BUDGET.
unsigned int scheduledMessagesTextLength(long excludeTimestamp) {
  unsigned int total = 0;
  for (int i = 0; i < scheduledMessages.size(); i++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(i);
    if (scheduledMessage.ScheduledDateTimeUnix != excludeTimestamp) {
      total += scheduledMessage.Message.length();
    }
  }
  return total;
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
        scheduledMessagesGeneration++;
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
    scheduledMessagesGeneration++;
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
  scheduledMessagesGeneration++;
}

//Remove and delete a existing scheduled message if found. Same SCHEDULE_SAVE_FAILED handling as
//addAndPersistScheduledMessage() - the message is then still scheduled.
int removeScheduledMessage(long scheduledDateTimeUnix) {
  if (!ensureScheduledMessagesComplete()) {
    return SCHEDULE_SAVE_FAILED;
  }
  //Find the existing scheduled message and delete it
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages.get(scheduledMessageIndex);

    if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln(F("Deleting Scheduled Message due to be shown: ") + scheduledMessage.Message);
      scheduledMessages.remove(scheduledMessageIndex);
      scheduledMessagesGeneration++;

      if (!writeScheduledMessagesToFile()) {
        reloadScheduledMessagesFromFile();
        return SCHEDULE_SAVE_FAILED;
      }
      return SCHEDULE_REMOVED;
    }
  }

  return SCHEDULE_NOT_FOUND;
}

//Show and remove any scheduled message whose time has passed
void checkScheduledMessages() {
  //Defer rather than silently consume a due message while parked for the night - showText()
  //would blank it anyway, but removing it from the queue here would lose it for good instead of
  //it firing for real once Quiet Hours end.
  if (isQuietHoursActive()) {
    return;
  }

  //A partially loaded list must not fire (and then save) anything - retry the load instead, at
  //most every 10s so a low-memory phase isn't made worse by reloading every second.
  if (!scheduledMessagesComplete) {
    static unsigned long lastReloadAttemptAt = 0;
    if (millis() - lastReloadAttemptAt < 10000) {
      return;
    }
    lastReloadAttemptAt = millis();
    if (!ensureScheduledMessagesComplete()) {
      return;
    }
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
        scheduledMessagesGeneration++;
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
        scheduledMessagesGeneration++;
        writeScheduledMessagesToFile();
      }
      break;
    }
  }
}

//One persisted element -> scheduledMessages, unconditionally (restoreScheduledMessage(), not
//addScheduledMessage() - a restored entry must never be rejected just because it's already due).
void restoreScheduledMessageFromJson(JsonVariant value) {
  //Only reachable if the persisted file itself somehow holds more than the cap -
  //addScheduledMessage() already enforces it for every new submission.
  if (scheduledMessages.size() >= SCHEDULED_MESSAGES_MAX) {
    return;
  }
  restoreScheduledMessage(String((const char*)(value["message"] | "")), value["scheduledDateTimeUnix"] | 0L, value["showIndefinitely"] | false);
}

//Loads the persisted scheduled messages - at boot, and to roll memory back to the file after a
//failed save. Read element by element (readJsonArrayFile()), so neither the whole file as a String
//nor a document holding every message is in memory at once. A missing file is simply an empty
//list; a corrupt one is replaced by an empty list. Out of memory while reading leaves the file
//alone - it's still valid, only this attempt to read it failed - and marks the list incomplete
//(see scheduledMessagesComplete).
void loadScheduledMessagesFromFile() {
  int result = readJsonArrayFile(scheduledMessagesPath, restoreScheduledMessageFromJson);
  scheduledMessagesComplete = (result != JSON_ARRAY_FILE_NO_MEMORY);
  if (result == JSON_ARRAY_FILE_INVALID) {
    SerialPrintln(F("Invalid scheduled messages file, scrapping and starting again"));
    scheduledMessages.clear();
    scheduledMessagesGeneration++;
    writeEmptyScheduledMessagesToFile();
  } else if (result == JSON_ARRAY_FILE_NO_MEMORY) {
    SerialPrintln(F("Out of memory while loading scheduled messages - keeping the file, list incomplete until a reload succeeds"));
  }
}

void reloadScheduledMessagesFromFile() {
  scheduledMessages.clear();
  scheduledMessagesGeneration++;
  loadScheduledMessagesFromFile();
}

//Retries an incomplete load (see scheduledMessagesComplete). True once memory matches the file.
bool ensureScheduledMessagesComplete() {
  if (!scheduledMessagesComplete) {
    reloadScheduledMessagesFromFile();
  }
  return scheduledMessagesComplete;
}

void fillScheduledMessageJson(int index, JsonDocument &element) {
  ScheduledMessage scheduledMessage = scheduledMessages.get(index);
  element["scheduledDateTimeUnix"] = scheduledMessage.ScheduledDateTimeUnix;
  element["message"] = scheduledMessage.Message;
  element["showIndefinitely"] = scheduledMessage.ShowIndefinitely;
}

//Save the scheduled messages to file - see writeJsonArrayFile() for why element by element and
//via a temp file. Returns false if they couldn't be saved; the previous file is then unchanged.
bool writeScheduledMessagesToFile() {
  if (!scheduledMessagesComplete) {
    SerialPrintln(F("Scheduled messages only partially loaded - not overwriting the file"));
    return false;
  }
  if (scheduledMessages.size() == 0) {
    SerialPrintln(F("No Scheduled Messages Left - Writing Empty to File"));
  }
  return writeJsonArrayFile(scheduledMessagesPath, scheduledMessages.size(), fillScheduledMessageJson);
}

//Write an empty array to the schedule messages file, used in case something goes terribly wrong
void writeEmptyScheduledMessagesToFile() {
  writeJsonArrayFile(scheduledMessagesPath, 0, fillScheduledMessageJson);
}
