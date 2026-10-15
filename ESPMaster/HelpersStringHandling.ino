//Constant-time string comparison, for password checks - avoids the timing side-channel a plain
//`==`'s early-exit byte compare would have. Always walks the longer of the two lengths and folds
//a length mismatch into the same accumulator, never returning early.
bool constantTimeEquals(const String &a, const String &b) {
  size_t lenA = a.length();
  size_t lenB = b.length();
  size_t maxLen = lenA > lenB ? lenA : lenB;
  uint8_t diff = (lenA != lenB) ? 1 : 0;
  for (size_t i = 0; i < maxLen; i++) {
    uint8_t byteA = i < lenA ? (uint8_t)a.charAt(i) : 0;
    uint8_t byteB = i < lenB ? (uint8_t)b.charAt(i) : 0;
    diff |= byteA ^ byteB;
  }
  return diff == 0;
}

//Percent-encodes a value for use as one field of an application/x-www-form-urlencoded body.
//Unreserved characters pass through unchanged, everything else becomes %XX. Space becomes %20,
//not '+' - needs no special-casing on the decoding side.
String urlEncodeForFormBody(String value) {
  String encoded;
  encoded.reserve(value.length());
  const char* hexDigits = "0123456789ABCDEF";
  for (int i = 0; i < (int)value.length(); i++) {
    uint8_t c = (uint8_t)value.charAt(i);
    bool isUnreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (isUnreserved) {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hexDigits[(c >> 4) & 0xF];
      encoded += hexDigits[c & 0xF];
    }
  }
  return encoded;
}

//UTF-8 ä/ö/ü/Ä/Ö/Ü -> the single-byte placeholders ($/&/#) the flaps have. Runs once in
//showText(), so every input path is handled alike and 2-byte sequences never shift flap positions.
String normalizeUmlautsForDisplay(const String &text) {
  String result = "";
  result.reserve(text.length());
  for (int i = 0; i < (int)text.length(); i++) {
    uint8_t currentByte = (uint8_t)text.charAt(i);
    if (currentByte == 0xC3 && i + 1 < (int)text.length()) {
      uint8_t nextByte = (uint8_t)text.charAt(i + 1);
      char replacement = 0;
      switch (nextByte) {
        case 0xA4: case 0x84: replacement = '$'; break; //ä / Ä
        case 0xB6: case 0x96: replacement = '&'; break; //ö / Ö
        case 0xBC: case 0x9C: replacement = '#'; break; //ü / Ü
      }
      if (replacement != 0) {
        result += replacement;
        i++; //consumed both bytes of this sequence - skip the second one
        continue;
      }
    }
    result += (char)currentByte;
  }
  return result;
}

//Centers the message in `width` (unitsAmount or combinedUnitsAmount()), padding with spaces. No
//default argument: the generated prototype would repeat it (redefinition error).
String centerString(String message, int width) {
  //message.length() is unsigned - cast first, or an over-long message wraps to a huge pad (OOM).
  int padTotal = width - (int)message.length();
  if (padTotal > 0) {
    //Takes care of the left side
    int leftSpaceAmount = padTotal / 2;
    for (int spaceIndex = 0; spaceIndex < leftSpaceAmount; spaceIndex++) {
      message = " " + message;
    }

    //Take care of the right side
    for (int spaceIndex = message.length(); spaceIndex < width; spaceIndex++) {
      message = message + " ";
    }
  }

  message = cleanString(message);

  return message;
}

String createRepeatingString(char character) {
  String newMessage = "";
  for (int unitIndex = 0; unitIndex < unitsAmount; unitIndex++) {
    newMessage.concat(character);
  }

  return newMessage;  
}

//Right-aligns the message in `width` (see centerString()).
String rightString(String message, int width) {
  //Same unsigned-underflow issue as centerString() above.
  int rightSpaceAmount = width - (int)message.length();
  for (int spaceIndex = 0; spaceIndex < rightSpaceAmount; spaceIndex++) {
    message = " " + message;
  }

  message = cleanString(message);

  return message;
}

//Left-aligns the message in `width` (see centerString()).
String leftString(String message, int width) {
  //Same unsigned-underflow issue as centerString() above.
  int leftSpaceAmount = width - (int)message.length();
  for (int spaceIndex = 0; spaceIndex < leftSpaceAmount; spaceIndex++) {
    message = message + " ";
  }

  message = cleanString(message);

  return message;
}

//Converts input string to uppercase
String cleanString(String message) {
  message.toUpperCase();
  
  return message;
}

int convertSpeed(const String &flapSpeed) {
  int flapSpeedInt;
  flapSpeedInt = flapSpeed.toInt();
  flapSpeedInt = map(flapSpeedInt, 1, 100, MIN_SPEED, MAX_SPEED);

  //map() extrapolates; stepperSpeed=0 makes the unit divide by zero and spin forever - so clamp.
  flapSpeedInt = constrain(flapSpeedInt, MIN_SPEED, MAX_SPEED);

  return flapSpeedInt;
}

//Word-wraps `sentenceIn` into lines of at most `width` characters (explicit "\n" breaks,
//hyphenated over-long words) and returns line `wantedLine` (0-based; "" if missing, -1 to count
//only), setting `lineCount`. Recomputed per line instead of keeping all lines, so a long message
//isn't held twice in memory. Lines aren't substrings of the input (hyphens added, whitespace
//collapsed). No default argument for `width`, see centerString().
String sentenceLine(const String &sentenceIn, int width, int wantedLine, int &lineCount) {
  String sentence = sentenceIn;
  String result;
  lineCount = 0;
  auto emitLine = [&](const String &line) {
    if (lineCount == wantedLine) {
      result = line;
    }
    lineCount++;
  };

  //Remove trailing and preceding whitespace
  sentence.trim();

  //Handle newline characters being passed up
  sentence.replace("\\n", "\n");

  //No sentence to start with a newline
  if (sentence.startsWith("\n")) {
    sentence = sentence.substring(1);
  }

  //Replace any double newlines with a single one, not allowing anymore then one
  while(sentence.indexOf("\n\n") != -1)
  {
    sentence.replace("\n\n", "\n");
  }

  //Remove pre-ceding space to a newline
  while(sentence.indexOf(" \n") != -1)
  {
    sentence.replace(" \n", "\n");
  }

  //Replace all double spaces
  while(sentence.indexOf("  ") != -1)
  {
    sentence.replace("  ", " ");
  }

  //Walk the space-separated words in place and process them into lines where possible
  String inProgressLine;
  int position = 0;
  int sentenceLength = sentence.length();
  while (position < sentenceLength)
  {
    //Find the next space - if none, take the rest of the string that is left
    int indexOfSpace = sentence.indexOf(' ', position);
    String wordItem = indexOfSpace == -1 ? sentence.substring(position) : sentence.substring(position, indexOfSpace);
    position = indexOfSpace == -1 ? sentenceLength : indexOfSpace + 1;
    bool isLastWord = position >= sentenceLength;

    //Only measure the word up until the first newline
    int indexOfFirstNewline = wordItem.indexOf('\n');

    //If the first character is not a newline see if we can fit it on the current line, otherwise don't bother
    if (indexOfFirstNewline != 0)
    {
      //Determine the word length up till the first newline as to see if it will fit on the current line
      int wordLength = indexOfFirstNewline == -1 ? wordItem.length() : indexOfFirstNewline;

      //If we cannot fit everything on the same line, start a new line
      if (inProgressLine != "" && inProgressLine.length() + wordLength + 1 > width)
      {
          emitLine(inProgressLine);
          inProgressLine = "";
      }

      //Only if we have a line that has letters in it, do we want a space on the end to split up the words
      if (inProgressLine.length() > 0)
      {
        inProgressLine += ' ';
      }
    }

    //Process the current word
    int currentWordLength = wordItem.length();
    for(int letterIndex = 0; letterIndex < currentWordLength; letterIndex++)
    {
      char wordItemLetter = wordItem[letterIndex];

      if (wordItemLetter == '\n')
      {
        //A word filling the width exactly before an explicit break was already flushed - don't
        //add an empty line.
        if (inProgressLine != "")
        {
          emitLine(inProgressLine);
          inProgressLine = "";
        }
      }
      else
      {
        //Hyphenate an over-long word - but not right before the word's own line break, and not on a
        //one-unit display (the hyphen would be a page of its own before every letter).
        bool moreWordContentFollows = (letterIndex < currentWordLength - 1) && (wordItem[letterIndex + 1] != '\n');
        if (width > 1 && inProgressLine.length() == width - 1 && moreWordContentFollows)
        {
          inProgressLine += '-';
          emitLine(inProgressLine);
          inProgressLine = "";
        }

        //Business as usual, add the letter onto the in progress line
        inProgressLine += wordItemLetter;
        if (inProgressLine.length() == width)
        {
          emitLine(inProgressLine);
          inProgressLine = "";
        }
      }
    }

    //Don't forget the last processed item
    if (isLastWord && inProgressLine != "")
    {
      emitLine(inProgressLine);
      inProgressLine = "";
    }
  }

  return result;
}

//Number of display lines sentenceLine() splits `sentence` into.
int countSentenceLines(const String &sentence, int width) {
  int lineCount;
  sentenceLine(sentence, width, -1, lineCount);
  return lineCount;
}

//Translates the user-facing format syntax (e.g. "HH:mm:ss", "dd.MM.yyyy") into ezTime's own
//PHP-date()-style tokens, which Timezone::dateTime() understands. Longest tokens first, so "yyyy"
//isn't chopped into four "y" matches. Text in single quotes is literal ("HH:mm 'Uhr'", '' = a quote
//itself), like Java's date patterns. Every other letter (and ezTime's escape characters '\' and
//'~') is escaped - ezTime reads most letters as tokens of its own.
String convertUserDateFormatToEzTime(String userFormat) {
  struct FormatToken { const char* from; const char* to; };
  static const FormatToken tokens[] = {
    {"yyyy", "Y"}, {"yy", "y"},
    {"MMMM", "F"}, {"MMM", "M"}, {"MM", "m"}, {"M", "n"},
    {"dd", "d"}, {"d", "j"},
    {"EEEE", "l"}, {"EEE", "D"},
    {"HH", "H"}, {"H", "G"},
    {"hh", "h"}, {"h", "g"},
    {"mm", "i"},
    {"ss", "s"},
  };

  String result = "";
  int i = 0;
  bool quoted = false;
  while (i < (int)userFormat.length()) {
    if (userFormat[i] == '\'') {
      if (i + 1 < (int)userFormat.length() && userFormat[i + 1] == '\'') {
        result += '\'';
        i += 2;
      } else {
        quoted = !quoted;
        i++;
      }
      continue;
    }
    if (quoted) {
      result += '\\';
      result += userFormat[i];
      i++;
      continue;
    }
    bool matched = false;
    for (unsigned int t = 0; t < sizeof(tokens) / sizeof(tokens[0]); t++) {
      String from = tokens[t].from;
      if (userFormat.substring(i, i + from.length()) == from) {
        result += tokens[t].to;
        i += from.length();
        matched = true;
        break;
      }
    }
    if (!matched) {
      char c = userFormat[i];
      if (isAlpha(c) || c == '\\' || c == '~') {
        result += '\\';
      }
      result += c;
      i++;
    }
  }
  return result;
}

//Used for ensuring a string is a number
bool isNumber(const String &str) {
  //Reject "" explicitly - strtol() would leave endPtr at the terminator and pass the check below.
  if (str.length() == 0) {
    return false;
  }

  char* endPtr;

  //10 specifies base 10 (decimal)
  strtol(str.c_str(), &endPtr, 10);

  //Check if the conversion reached the end of the string
  return (*endPtr == '\0');
}

//Parses a fixed "HH:mm" time-of-day string into minutes-since-midnight (0-1439). Returns -1 for
//anything malformed - used for Quiet Hours' start/end times.
int parseTimeOfDayToMinutes(String value) {
  if (value.length() != 5 || value.charAt(2) != ':') {
    return -1;
  }

  //Digits only - isNumber() (strtol) would also let " 7" or "+7" through.
  for (int i = 0; i < 5; i++) {
    if (i != 2 && !isDigit(value.charAt(i))) {
      return -1;
    }
  }
  String hourPart = value.substring(0, 2);
  String minutePart = value.substring(3, 5);

  int hour = hourPart.toInt();
  int minute = minutePart.toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return -1;
  }

  return hour * 60 + minute;
}

//Validates a candidate mDNS/DNS hostname label (the "split-flap" in "split-flap.local"). Standard
//DNS label rules: 1-63 chars, only letters/digits/hyphens, must not start or end with a hyphen
//(RFC 1035/1123) - ESP8266mDNS itself doesn't validate this.
bool isValidMdnsHostname(String hostname) {
  if (hostname.length() == 0 || hostname.length() > 63) {
    return false;
  }
  if (hostname.charAt(0) == '-' || hostname.charAt(hostname.length() - 1) == '-') {
    return false;
  }
  for (unsigned int i = 0; i < hostname.length(); i++) {
    char c = hostname.charAt(i);
    bool isLetter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    bool isDigit = (c >= '0' && c <= '9');
    if (!isLetter && !isDigit && c != '-') {
      return false;
    }
  }
  return true;
}

//Extension address: IPv4 or hostname, optionally ":port". Rejects paths, queries, credentials and
//spaces - the value goes straight into "http://<host>/api/...".
bool isValidExtensionHost(String host) {
  int colon = host.indexOf(':');
  if (colon != -1) {
    String port = host.substring(colon + 1);
    if (port.length() == 0 || port.length() > 5) {
      return false;
    }
    for (unsigned int i = 0; i < port.length(); i++) {
      if (port.charAt(i) < '0' || port.charAt(i) > '9') {
        return false; //digits only - isNumber() would also let a sign or leading space through
      }
    }
    if (port.toInt() < 1 || port.toInt() > 65535) {
      return false;
    }
    host = host.substring(0, colon);
  }
  if (host.length() == 0 || host.length() > 253) {
    return false;
  }
  int labelStart = 0;
  while (true) {
    int dot = host.indexOf('.', labelStart);
    if (!isValidMdnsHostname(host.substring(labelStart, dot == -1 ? host.length() : dot))) {
      return false;
    }
    if (dot == -1) {
      return true;
    }
    labelStart = dot + 1;
  }
}