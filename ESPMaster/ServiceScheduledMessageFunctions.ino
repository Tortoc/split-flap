//Replace/add a scheduled message and persist it. SCHEDULE_SAVE_FAILED: saving failed (e.g. out of
//memory); the list is reloaded from the untouched file and the message counts as not scheduled.
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

  //Reject a past-due timestamp before touching the list - otherwise a resubmission could delete the
  //matching valid entry. timezone.now() isn't real UTC, see getRealUtcNow().
  if (scheduledDateTimeUnix > getRealUtcNow()) {
    //Find the existing scheduled message and replace it if one exists.
    //.get(), not operator[]: the library's operator[] dereferences NULL on a stale index.
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

//Restores a persisted message at boot without rejecting past-due timestamps -
//checkScheduledMessages() fires those on its next tick. A loaded list has no duplicates.
void restoreScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  scheduledMessages.add({scheduledText, scheduledDateTimeUnix, showIndefinitely});
  scheduledMessagesGeneration++;
}

//Remove a scheduled message. On SCHEDULE_SAVE_FAILED it stays scheduled (see above).
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
  //During Quiet Hours, defer instead of consuming a due message - it would otherwise be lost.
  if (isQuietHoursActive()) {
    return;
  }

  //Never fire (and save) from a partially loaded list; retry the load at most every 10s.
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

      //Too many lines can never be shown; keeping it would block every other due message forever.
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

      //Only dequeue on success, otherwise a busy display would silently lose the message.
      if (shown) {
        scheduledMessages.remove(scheduledMessageIndex);
        scheduledMessagesGeneration++;
        writeScheduledMessagesToFile();
      }
      break;
    }
  }
}

//One persisted element -> scheduledMessages via restoreScheduledMessage() (no past-due check).
void restoreScheduledMessageFromJson(JsonVariant value) {
  //Only if the file holds more than the cap - new submissions are already capped.
  if (scheduledMessages.size() >= SCHEDULED_MESSAGES_MAX) {
    return;
  }
  restoreScheduledMessage(String((const char*)(value["message"] | "")), value["scheduledDateTimeUnix"] | 0L, value["showIndefinitely"] | false);
}

//Loads the persisted scheduled messages - at boot and to roll back after a failed save. Read element
//by element (readJsonArrayFile()) to keep memory low. Missing file = empty list, corrupt file =
//replaced by an empty list. Out of memory while reading leaves the file alone and marks the list
//incomplete (scheduledMessagesComplete).
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
