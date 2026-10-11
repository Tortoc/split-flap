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

//Converts UTF-8 ä/ö/ü/Ä/Ö/Ü byte sequences to the internal single-byte placeholder codes ($/&/#)
//that the physical letters[] array (ESPMaster.ino) actually has flaps for. Runs once, centrally,
//in showText() - so every input path (Rotation, scheduled messages, /api/text) gets the same
//handling, and raw 2-byte UTF-8 sequences never shift later characters' flap positions.
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

//Aligns string on center of array and fills empty chars with spaces. Callers pass unitsAmount or
//combinedUnitsAmount() for a primary device with reachable extensions. No default value here
//deliberately: Arduino's auto-prototype generator re-emits this signature as a forward
//declaration, and a default argument in two places is a C++ redefinition error.
String centerString(String message, int width) {
  //message.length() is unsigned - cast to int first, or a too-long message wraps the subtraction
  //to a huge positive pad amount instead of negative, driving the loop below into an OOM crash.
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

//Aligns string on right side of array and fills empty chars with spaces. width - see
//centerString()'s own comment.
String rightString(String message, int width) {
  //Same unsigned-underflow issue as centerString() above.
  int rightSpaceAmount = width - (int)message.length();
  for (int spaceIndex = 0; spaceIndex < rightSpaceAmount; spaceIndex++) {
    message = " " + message;
  }

  message = cleanString(message);

  return message;
}

//Aligns string on left side of array and fills empty chars with spaces. width - see
//centerString()'s own comment.
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

  //map() extrapolates, doesn't clamp - an out-of-range result maps to stepperSpeed=0 on the Unit,
  //which divides by zero and spins the motor loop forever. Clamp defensively regardless of input.
  flapSpeedInt = constrain(flapSpeedInt, MIN_SPEED, MAX_SPEED);

  return flapSpeedInt;
}

//Word-wraps `sentenceIn` into display lines of at most `width` characters (explicit "\n" breaks,
//"-"-hyphenated over-long words) and returns only line number `wantedLine` (0-based; "" if there is
//no such line, -1 to just count), setting `lineCount` to the total. Computed again for every line
//instead of keeping a list of all lines: the display job only ever needs one line at a time, and a
//500-character message no longer sits in memory a second time as words plus lines
//(CODE_REVIEW_V7.md B). The lines aren't plain substrings of the input - hyphens get inserted and
//whitespace collapsed - so positions/lengths into the original text wouldn't be enough.
//
//Same algorithm as the former processSentenceToLines() (host-tested line for line against it),
//just walking the words in place and handing each finished line to emitLine below.
//width - see centerString()'s own comment (same reasoning, same no-default-argument caveat).
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

    //If the first characeter is not a newline see if we can fit it on the current line, otherwise don't bother
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
        //Guard against a spurious empty line: a word filling the display width EXACTLY, right
        //before an explicit line break, already flushed inProgressLine one letter earlier via the
        //width-length check below - adding it again here would insert an empty line.
        if (inProgressLine != "")
        {
          emitLine(inProgressLine);
          inProgressLine = "";
        }
      }
      else
      {
        //If we need to break up the word as its too long, then we need to add a "-" - but NOT if
        //the very next character is just this word's own trailing line break (reaching the
        //display width one letter before an explicit newline isn't genuinely needing to break).
        bool moreWordContentFollows = (letterIndex < currentWordLength - 1) && (wordItem[letterIndex + 1] != '\n');
        if (inProgressLine.length() == width - 1 && moreWordContentFollows)
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

    //Don't forgot the last processed item...
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
//isn't chopped into four "y" matches.
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
  while (i < (int)userFormat.length()) {
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
      result += userFormat[i];
      i++;
    }
  }
  return result;
}

//Used for ensuring a string is a number
bool isNumber(const String &str) {
  //An empty string must be rejected explicitly - strtol("", &endPtr, 10) consumes no digits but
  //still leaves endPtr at the terminator, so the end-of-string check below would trivially pass.
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

  String hourPart = value.substring(0, 2);
  String minutePart = value.substring(3, 5);
  if (!isNumber(hourPart) || !isNumber(minutePart)) {
    return -1;
  }

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