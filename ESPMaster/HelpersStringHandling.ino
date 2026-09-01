//Aligns string on center of array and fills empty chars with spaces
String centerString(String message) {
  //message.length() is unsigned - cast to int first, or a too-long message wraps the subtraction
  //to a huge positive pad amount instead of negative, driving the loop below into an OOM crash.
  int padTotal = unitsAmount - (int)message.length();
  if (padTotal > 0) {
    //Takes care of the left side
    int leftSpaceAmount = padTotal / 2;
    for (int spaceIndex = 0; spaceIndex < leftSpaceAmount; spaceIndex++) {
      message = " " + message;
    }

    //Take care of the right side
    for (int spaceIndex = message.length(); spaceIndex < unitsAmount; spaceIndex++) {
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

//Aligns string on right side of array and fills empty chars with spaces
String rightString(String message) {
  //Same unsigned-underflow issue as centerString() above.
  int rightSpaceAmount = unitsAmount - (int)message.length();
  for (int spaceIndex = 0; spaceIndex < rightSpaceAmount; spaceIndex++) {
    message = " " + message;
  }

  message = cleanString(message);

  return message;
}

//Aligns string on left side of array and fills empty chars with spaces
String leftString(String message) {
  //Same unsigned-underflow issue as centerString() above.
  int leftSpaceAmount = unitsAmount - (int)message.length();
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

int convertSpeed(String flapSpeed) {
  int flapSpeedInt;
  flapSpeedInt = flapSpeed.toInt();
  flapSpeedInt = map(flapSpeedInt, 1, 100, MIN_SPEED, MAX_SPEED);

  //map() extrapolates, doesn't clamp - an out-of-range result maps to stepperSpeed=0 on the Unit,
  //which divides by zero and spins the motor loop forever. Clamp defensively regardless of input.
  flapSpeedInt = constrain(flapSpeedInt, MIN_SPEED, MAX_SPEED);

  return flapSpeedInt;
}

LList<String> processSentenceToLines(String sentence) {    
  SerialPrintln("Processing Sentence to lines");
  
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
    
  // Split the string into substrings based on spaces
  LList<String> words; 
  while (sentence.length() > 0)
  {
    //Find the next space
    int indexOfSpace = sentence.indexOf(' ');

    //If not space, take the rest of the string that is left
    //Else, process the part of the string
    if (indexOfSpace == -1)
    {
      words.add(sentence);
      break;
    }
    else
    {
      words.add(sentence.substring(0, indexOfSpace));
      sentence = sentence.substring(indexOfSpace + 1);
    }
  }

  //Now process the words into lines where possible
  LList<String> lines;
  String inProgressLine;
  while(words.size() != 0) 
  {
    //Always get the first word
    String wordItem = words[0];

    //Only measure the word up until the first newline
    int indexOfFirstNewline = wordItem.indexOf('\n');

    //If the first characeter is not a newline see if we can fit it on the current line, otherwise don't bother
    if (indexOfFirstNewline != 0) 
    {
      //Determine the word length up till the first newline as to see if it will fit on the current line
      int wordLength = indexOfFirstNewline == -1 ? wordItem.length() : indexOfFirstNewline;

      //If we cannot fit everything on the same line, start a new line
      if (inProgressLine != "" && inProgressLine.length() + wordLength + 1 > unitsAmount)
      {
          lines.add(inProgressLine);
          inProgressLine = "";
      }

      //Only if we have a line that has letters in it, do we want a space on the end to split up the words
      if (inProgressLine.length() > 0)
      {
        inProgressLine = inProgressLine + " ";
      }
    }

    //Process the current word
    int currentWordLength = wordItem.length();
    for(int letterIndex = 0; letterIndex < currentWordLength; letterIndex++)
    {
      char wordItemLetter = wordItem[letterIndex];

      if (wordItemLetter == '\n')
      {
        lines.add(inProgressLine);
        inProgressLine = "";
      }
      else
      {
        //If we need to break up the word as its too long, then we need to add a "-"
        if (inProgressLine.length() == unitsAmount - 1 && letterIndex < currentWordLength - 1)
        {
          inProgressLine = inProgressLine + "-";
          lines.add(inProgressLine);
          inProgressLine = "";
        }

        //Business as usual, add the letter onto the in progress line
        inProgressLine = inProgressLine + wordItemLetter;
        if (inProgressLine.length() == unitsAmount)
        {  
          lines.add(inProgressLine);
          inProgressLine = "";
        }
      }
    }
    
    //Remove now we've processed that word
    words.remove(0);
    
    //Don't forgot the last processed item...
    if (words.size() == 0 && inProgressLine != "")
    {
      lines.add(inProgressLine);
      inProgressLine = "";
    }
  }

  return lines;
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
bool isNumber(String str) {
  char* endPtr;

  //10 specifies base 10 (decimal)
  strtol(str.c_str(), &endPtr, 10);  

  //Check if the conversion reached the end of the string
  return (*endPtr == '\0');
}