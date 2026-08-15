//Used for local development use
const localDevelopment = false;

//Various variables
var unitCount = 0;
var timezoneOffset = 0;

//Used for submission!
const form = document.getElementById('form');
form.onsubmit = function () {
	//Show loading icon
	var containerSubmit = document.getElementById('containerSubmit');
	
	const loadingIconContainer = document.createElement("div");
	loadingIconContainer.className = "lds-facebook";

	for(var index = 0; index < 3; index++) {
		loadingIconContainer.appendChild(document.createElement("div"));
	}

	containerSubmit.replaceWith(loadingIconContainer);

	if (localDevelopment) {
		setTimeout(function() {
			location.reload();
		}, 3000);
		
		return false;
	}
	else {
		const deviceMode = document.querySelector('input[name="deviceMode"]:checked').value;
		const tzOffset = timezoneOffset * 60000;

		switch(deviceMode) {
			case "text":
				//Convert characters which don't translate directly, replaces ä, ö, ü with unused unicode characters #, $, &
				var inputTextValue = document.getElementById('inputText').value;
				inputTextValue = inputTextValue.replace(/ä/gi, '$');
				inputTextValue = inputTextValue.replace(/ö/gi, '&');
				inputTextValue = inputTextValue.replace(/ü/gi, '#');
				document.getElementById('inputText').value = inputTextValue;

				//Set the hidden date time to UNIX
				var currentScheduledDateTimeText = document.getElementById('inputScheduledDateTime').value;

				//Take into account the timezone offset when we generate the unix timestamp
				var currentScheduledDateTime = new Date(currentScheduledDateTimeText);
				var time = Math.floor((currentScheduledDateTime.getTime() - tzOffset) / 1000);
				document.getElementById('inputHiddenScheduledDateTimeUnix').value = time;

				break;
			case "countdown":
				//Set the hidden date time to UNIX
				var currentCountdownDateTimeText = document.getElementById('inputCountdownDateTime').value;

				//Take into account the timezone offset when we generate the unix timestamp
				var currentCountdownDateTime = new Date(currentCountdownDateTimeText);
				var time = Math.floor((currentCountdownDateTime.getTime() - tzOffset) / 1000);
				document.getElementById('inputHiddenCountdownDateTimeUnix').value = time;

				break;
		}

		//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #68): this used to let the browser perform a real
		//POST + full-page navigation here (the server responded with a redirect back to "/" - a
		//classic Post/Redirect/Get pattern to dodge the "resubmit form?" browser prompt on refresh,
		//no longer needed once nothing navigates at all). Every Enter-to-submit or Submit-click
		//caused a visible full-page reload - reported by the user as the page "blinking". Converted
		//to AJAX: same field values (via FormData, so every existing name="..." input is picked up
		//automatically, nothing to keep in sync by hand), sent to the same endpoint, page never
		//navigates.
		var formData = new FormData(form);
		var xhrRequest = new XMLHttpRequest();
		xhrRequest.onreadystatechange = function () {
			if (this.readyState == 4) {
				restoreSubmitButton(loadingIconContainer);

				if (this.status == 200) {
					//Matches the old reload-clears-it behavior - a fresh page load never restored
					//previously typed text either, since index.html doesn't carry a stored value.
					document.getElementById('inputText').value = "";
					updateCharacterCount();
					//Refresh everything else (mode/alignment/speed/countdown/scheduled messages/
					//"last received") from the server's now-updated state.
					loadPage();
				} else {
					showBannerMessage(this.responseText || `
						Something went wrong during submission. Feel free to try again, ensure that you have entered valid information.
						<br>
						Ensure things like dates provided for schedules/countdowns are in the future.
					`);
				}
			}
		};
		xhrRequest.onerror = function () {
			restoreSubmitButton(loadingIconContainer);
			showBannerMessage("Network error while submitting.");
		};
		xhrRequest.open("POST", "/", true);
		xhrRequest.send(formData);

		return false;
	}
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #68): the loading spinner used to replace the Submit
//button permanently - fine when a real page reload followed right after (a fresh page load
//recreates it), not fine now that the page stays alive and the user needs to be able to submit
//again.
function restoreSubmitButton(loadingIconContainer) {
	var freshContainer = document.createElement("div");
	freshContainer.id = "containerSubmit";
	var submitInput = document.createElement("input");
	submitInput.type = "submit";
	submitInput.value = "Submit";
	freshContainer.appendChild(submitInput);
	loadingIconContainer.replaceWith(freshContainer);
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #59): used to also append to an on-page "Page Load
//Errors & Status" panel (removed - see the comment in index.html for why). Kept as a
//console-only logger since the many call sites throughout the /settings loading flow below are
//still genuinely useful when debugging with devtools open, and this way costs nothing on a
//normal load.
function logPageLoadStatus(message, isError) {
	var timestamp = new Date().toLocaleTimeString();
	var logEntry = "[" + timestamp + "] " + message;

	if (isError) {
		console.error(logEntry);
	} else {
		console.log(logEntry);
	}
}

// Catch JavaScript errors
window.addEventListener('error', function(e) {
	logPageLoadStatus("JavaScript Error: " + e.message + " at " + e.filename + ":" + e.lineno, true);
});

// Catch unhandled promise rejections
window.addEventListener('unhandledrejection', function(e) {
	logPageLoadStatus("Unhandled Promise Rejection: " + e.reason, true);
});

// Retrieve current Split-Flap settings when the page loads/refreshes
window.addEventListener('load', function() {
	logPageLoadStatus("Page load event fired", false);
	loadPage();
});

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #68): loadPage() used to also check for an
//'invalid-submission' URL param here (see BUGFIX_LOG.md #67 for the same story about a since-
//removed 'is-resetting-units' param) - both were only ever set by a server redirect after a real
//<form> POST navigation. Neither endpoint redirects anymore (both now respond to AJAX calls
//instead, see form.onsubmit and triggerResetUnits() in admin.js), so both params are permanently
//unreachable now - removed rather than left as dead branches.

// Request and retrieve settings from ESP-01s filesystem
function loadPage() {
	//Set date time fields to be a minimum of todays date/time add 1 minute
	var tzOffset = timezoneOffset * 60000;
	document.querySelectorAll('input[type="datetime-local"]').forEach((dateTimeElement) => {
		var currentDateTime = convertDateToString((new Date(Date.now() - tzOffset + 60000)));
		dateTimeElement.value = dateTimeElement.min = currentDateTime;
	});

	if (localDevelopment) {
		setSpeed("80");
		setSavedMode("text");
		setAlignment("left");
		setUnitCount("10");
		setLastReceivedMessage(new Date().toLocaleString());
		setWiFiStatus("Connected", -65, "192.168.1.100"); // Mock WiFi status for development
		setCountdownDate((Date.now() / 1000) + (24 * 60 * 60));
		showScheduledMessages([
			{
				"scheduledDateTimeUnix": 1690134480,
				"message": "Test Message 1",
				"showIndefinitely": false
			},
		]);

		setTimeout(function() {
			showContent();
		}, 1000);
	}
	else {
		logPageLoadStatus("Starting /settings request...", false);
		var settingsRequestStart = Date.now();
		
		var xhrRequest = new XMLHttpRequest();
		
		// Set timeout (30 seconds)
		var timeoutId = setTimeout(function() {
			if (xhrRequest.readyState !== 4) {
				xhrRequest.abort();
				var elapsed = ((Date.now() - settingsRequestStart) / 1000).toFixed(1);
				logPageLoadStatus("ERROR: /settings request timed out after " + elapsed + " seconds", true);
				showContent(); // Show page anyway, even if settings failed
			}
		}, 30000);
		
		xhrRequest.onreadystatechange = function () {
			if (this.readyState == 4) {
				clearTimeout(timeoutId);
				var elapsed = ((Date.now() - settingsRequestStart) / 1000).toFixed(1);
				
				if (this.status == 200) {
					logPageLoadStatus("/settings request completed successfully in " + elapsed + "s", false);
					try {
						var responseObject = JSON.parse(this.responseText);
						logPageLoadStatus("JSON parsed successfully", false);
						
						timezoneOffset = responseObject.timezoneOffset;

						setSpeed(responseObject.flapSpeed);
						setSavedMode(responseObject.deviceMode);
						setAlignment(responseObject.alignment);
						setUnitCount(responseObject.unitCount);
						setCountdownDate(responseObject.countdownToDateUnix);
						setLastReceivedMessage(responseObject.lastTimeReceivedMessageDateTime);
						setWiFiStatus(responseObject.wifiStatus, responseObject.wifiRssi, responseObject.wifiIp);

						if (responseObject.scheduledMessages) {
							showScheduledMessages(responseObject.scheduledMessages);
						}

						showContent();
					} catch (e) {
						logPageLoadStatus("ERROR: Failed to parse JSON response: " + e.message, true);
						logPageLoadStatus("Response was: " + this.responseText.substring(0, 200), true);
						showContent(); // Show page anyway
					}
				} else {
					logPageLoadStatus("ERROR: /settings request failed with status " + this.status + " after " + elapsed + "s", true);
					logPageLoadStatus("Response: " + (this.responseText || "No response"), true);
					showContent(); // Show page anyway
				}
			} else if (this.readyState == 1) {
				logPageLoadStatus("/settings request opened, waiting for response...", false);
			} else if (this.readyState == 2) {
				logPageLoadStatus("/settings request received headers (status: " + this.status + ")", false);
			} else if (this.readyState == 3) {
				logPageLoadStatus("/settings request loading...", false);
			}
		};
		
		xhrRequest.onerror = function() {
			clearTimeout(timeoutId);
			var elapsed = ((Date.now() - settingsRequestStart) / 1000).toFixed(1);
			logPageLoadStatus("ERROR: Network error on /settings request after " + elapsed + "s", true);
			showContent(); // Show page anyway
		};

		xhrRequest.open("GET", "/settings", true);
		xhrRequest.send();
		logPageLoadStatus("/settings request sent", false);
	}
}

// Shows a message up top of the page should the server request one to be shown
function showBannerMessage(message, hideAfterDuration) {
	var bannerMessageElement = document.getElementById('bannerMessage'); 
	bannerMessageElement.innerHTML = message;

	bannerMessageElement.classList.remove("hidden");

	if (hideAfterDuration) {
		setTimeout(function() {
			bannerMessageElement.classList.add("hidden");
		}, 7500);
	}
}

//Ongoing show how many characters are being used
function updateCharacterCount() {
	var inputText = document.getElementById('inputText').value;
	var length = inputText.replaceAll("\\n", "").length;

	var labelCharacterCount = document.getElementById("labelCharacterCount");
	var labelLineCount = document.getElementById("labelLineCount");

	labelCharacterCount.innerHTML = length;
	//unitCount can be 0 if no units were detected on startup - avoid Infinity/NaN in that case
	var lineWidthEstimate = unitCount > 0 ? Math.ceil(length / unitCount) : 0;
	labelLineCount.innerHTML = lineWidthEstimate + inputText.split("\\n").length - 1;
}

//Easy add a newline
function addNewline() {
	var inputTextElement = document.getElementById('inputText'); 
	var textWithNewline = inputTextElement.value + "\\n";
	inputTextElement.value = textWithNewline;

	updateCharacterCount();
}

//Send message to delete a message
function deleteScheduledMessage(id, message) {
	var confirmDeletion = confirm(`Delete Message '${message}'?`);
	if (!confirmDeletion) {
		return false;
	}

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		//Reload the page
		if (this.readyState == 4 && this.status == 202) {
			window.location.reload();
		}
	};

	xhr.open("DELETE", `/scheduled-message/remove?id=${id}`, true);
	xhr.send();
}

//Updates slider value while sliding
function updateSpeedSlider() {
	var sliderValue = document.getElementById("rangeFlapSpeed").value;
	document.getElementById("rangeFlapSpeedValue").innerHTML = sliderValue + " %";
}

//Sets mode by checking corresponding radio button/tab
function setSavedMode(mode) {
	switch (mode) {
		case "text":
			document.getElementById("modeText").checked = true;
			break;
		case "date":
			document.getElementById("modeDate").checked = true;
			break;
		case "clock":
			document.getElementById("modeClock").checked = true;
			break;
		case "countdown":
			document.getElementById("modeCountdown").checked = true;
			break;
	}

	setDeviceModeTab(mode);
}

//Shows/hides the tab associated with the device mode
function setDeviceModeTab(mode) {
	document.querySelectorAll('.tab').forEach(function(tab) {
		if (!tab.classList.contains("hidden")) {
			tab.classList.add("hidden");
		}
	});

	var tabName = `tab-${mode}`;
	var tab = document.getElementById(tabName);
	if (tab !== null) {
		tab.classList.remove("hidden");
	}
}

//Sets flap speed by setting the ranges
function setSpeed(speed) {
	document.getElementById("rangeFlapSpeedValue").innerHTML = speed + " %";
	document.getElementById("rangeFlapSpeed").value = speed;
}

//Sets alignment by checking corresponding radio button
function setAlignment(alignment) {
	switch (alignment) {
		case "left":
			document.getElementById("radioLeft").checked = true;
			break;
		case "center":
			document.getElementById("radioCenter").checked = true;
			break;
		case "right":
			document.getElementById("radioRight").checked = true;
			break;
	}
}

//Sets the version on the UI just for awareness
function setUnitCount(count) {
	document.getElementById("labelUnits").innerHTML = count;
	unitCount = count;
}

//Sets the version on the UI just for awareness
function setCountdownDate(dateUnix) {
	//Set date fields to be a minimum of tomorrows date
	var currentCountdownDate = document.getElementById('inputCountdownDateTime');
	var tzOffset = timezoneOffset * 60000;

	var currentDate = (new Date(Date.now() - tzOffset));
	var nextDayDate = new Date();
	nextDayDate.setDate(currentDate.getDate() + 1);

	//If one has been set and it is not exceeded
	if (dateUnix !== 0) {
		var countdownDate = new Date(dateUnix * 1000);
		if (countdownDate >= currentDate) {
			currentCountdownDate.value = convertDateToString(countdownDate);

			if (countdownDate - currentDate < 24000) {
				currentCountdownDate.min = convertDateToString(nextDayDate);
				return;
			}
		}
	}
	else {
		//Set date fields to be a minimum of tomorrows date	
		currentCountdownDate.value = convertDateToString(nextDayDate);
	}

	currentCountdownDate.min = convertDateToString(nextDayDate);
}

function convertDateToString(dateTime) {
	const year = dateTime.getFullYear();
	const month = String(dateTime.getMonth() + 1).padStart(2, '0');
  	const day = String(dateTime.getDate()).padStart(2, '0');
	  
	const hours = String(dateTime.getHours()).padStart(2, '0');
	const minutes = String(dateTime.getMinutes()).padStart(2, '0');

	return `${year}-${month}-${day}T${hours}:${minutes}`;
}

//Sets the last received post message to the server
function setLastReceivedMessage(time) {
	const timeMessage = time == "" ? "N/A" : time;
	document.getElementById("labelLastMessageReceived").innerHTML = timeMessage;
}

//Sets the WiFi status with signal strength and qualitative assessment
function setWiFiStatus(status, rssi, ip) {
	var labelWiFiStatus = document.getElementById("labelWiFiStatus");
	var statusText = "";
	var statusColor = "#888";
	
	if (status === "Connected" && rssi !== undefined && rssi !== null) {
		// Determine qualitative assessment based on RSSI
		var quality = "";
		var qualityColor = "";
		
		if (rssi >= -50) {
			quality = "Excellent";
			qualityColor = "#4caf50"; // Green
		} else if (rssi >= -60) {
			quality = "Very Good";
			qualityColor = "#8bc34a"; // Light green
		} else if (rssi >= -70) {
			quality = "Good";
			qualityColor = "#cddc39"; // Lime
		} else if (rssi >= -80) {
			quality = "Fair";
			qualityColor = "#ffc107"; // Amber
		} else if (rssi >= -90) {
			quality = "Weak";
			qualityColor = "#ff9800"; // Orange
		} else {
			quality = "Very Weak";
			qualityColor = "#f44336"; // Red
		}
		
		statusText = status + " (" + rssi + " dBm) - <span style='color: " + qualityColor + "; font-weight: bold;'>" + quality + "</span>";
		statusColor = "#4caf50"; // Green for connected
	} else if (status === "Disconnected") {
		statusText = "Disconnected";
		statusColor = "#f44336"; // Red
	} else {
		statusText = "Unknown";
		statusColor = "#888"; // Gray
	}
	
	labelWiFiStatus.innerHTML = statusText;
	labelWiFiStatus.style.color = statusColor;
}

//Used for scheduling messages
function showHideScheduledMessageInput() {
	var scheduleOptionsElement = document.getElementById("divScheduleOptions");
	var checkboxScheduled = document.getElementById("inputCheckboxScheduleEnabled");

	if (checkboxScheduled.checked) {
		scheduleOptionsElement.classList.remove("hidden")
	}
	else {
		scheduleOptionsElement.classList.add("hidden")
	}
}

//BUG FIX, 2026-08-15: showHideResetWifiSettingsAction()/showHideOtaUpdateAction() used to live
//here, controlling the Reset WiFi / OTA Update links that were part of index.html's old
//"Actions" card. Both moved to admin.html (see BUGFIX_LOG.md #57) along with the equivalent
//show/hide logic - see loadSystemActionVisibility() in admin.js.

//Formats and displays all scheduled messages in a "nice" format
function showScheduledMessages(scheduledMessages) {
	var elementMessageCount = document.getElementById("spanScheduledMessageCount");
	elementMessageCount.innerText = scheduledMessages.length;

	//Closest to being shown first
	scheduledMessages = scheduledMessages.sort((a, b) => a.scheduledDateTimeUnix - b.scheduledDateTimeUnix);

	for (var scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.length; scheduledMessageIndex++) {
		var scheduledMessage = scheduledMessages[scheduledMessageIndex];

		//Create a container for a message
		var messageElement = document.createElement("div");
		messageElement.className = "message";

		//Create a element to show the time
		var timeElement = document.createElement("div");
		timeElement.className = "time";
		timeElement.innerText = new Date((scheduledMessage.scheduledDateTimeUnix * 1000) + (timezoneOffset * 60000)).toString().slice(0, -34);
		
		//Create a element to show the indefinite...ness...
		//SECURITY FIX, 2026-08-13: scheduledMessage.message is user-supplied text (via the
		//normal "schedule a message" form) and used to go straight into innerHTML unescaped -
		//a message like "<img src=x onerror=alert(1)>" would execute in anyone's browser who
		//later loaded this page. Escaped here now.
		var message = `<b>Message:</b> ${scheduledMessage.message.trim() == "" ? "<Blank>" : escapeHtml(scheduledMessage.message)}`
		var isIndefinitely = `<b>Shown Indefinitely:</b> ${scheduledMessage.showIndefinitely ? "Yes" : "No"}`;

		//Create a element to show the text
		var textElement = document.createElement("div");
		textElement.className = "text";
		textElement.innerHTML = `${message}<br>${isIndefinitely}`;

		//Create a remove button
		var actionElement = document.createElement("div");
		var actionButtonElement = document.createElement("span");
		actionElement.className = "action";
		actionButtonElement.className = "remove-button";
		actionButtonElement.innerText = "Remove";
		//SECURITY FIX, 2026-08-13: this used to build the handler as
		//`onclick="deleteScheduledMessage(..., '${scheduledMessage.message}')"` - splicing
		//unescaped user text directly into a JS string literal inside an HTML attribute. A
		//message containing a single quote (e.g. "test'); alert('xss") would break out of the
		//string and inject arbitrary JavaScript that runs for anyone viewing this page - a
		//classic attribute-injection XSS. addEventListener with a closure sidesteps the whole
		//class of bug: the message never passes through any string/HTML parsing at all.
		actionButtonElement.addEventListener('click', (function (dateTimeUnix, messageText) {
			return function () { deleteScheduledMessage(dateTimeUnix, messageText); };
		})(scheduledMessage.scheduledDateTimeUnix, scheduledMessage.message));
		actionElement.appendChild(actionButtonElement);

		//Add all the elements to the message
		messageElement.appendChild(timeElement);
		messageElement.appendChild(textElement);
		messageElement.appendChild(actionElement);

		//Append to the message container
		var container = document.getElementById("containerScheduledMessages");
		container.appendChild(messageElement);
	}
}

function showContent() {
	var elementInitialLoading = document.getElementById("initialLoading");
	var elementContent = document.getElementById("loadedContent");

	elementInitialLoading.classList.add("hidden");
	elementContent.classList.remove("hidden");
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #61): refreshLog()/displaySerialLog()/toggleSerialLog()/
//copySerialLog() used to live here, driving the "Serial Log" card - moved to admin.js along with
//the card itself (now on the Administration page, on user request).

// Helper function to escape HTML
function escapeHtml(text) {
	var map = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#039;'
	};
	return text.replace(/[&<>"']/g, function(m) { return map[m]; });
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #59): loadDebugLog()/displayDebugLog()/toggleDebugLog()/
//togglePageLoadLog()/copyDebugLog() used to live here, driving the two removed page-load debug
//panels (see the comment in index.html). loadDebugLog() in particular polled /log every 500ms
//for the entire page-load phase, competing with the actual /settings request - removed along
//with the panels rather than just hidden, since nothing else in this file calls them anymore.

// Kebab (3-dot) menu, added 2026-08-15 - see BUGFIX_LOG.md #57.
function toggleKebabMenu() {
	var dropdown = document.getElementById("kebabMenuDropdown");
	var button = document.getElementById("btnKebabMenu");
	var isOpen = !dropdown.classList.contains("hidden");

	if (isOpen) {
		dropdown.classList.add("hidden");
		button.setAttribute("aria-expanded", "false");
	} else {
		dropdown.classList.remove("hidden");
		button.setAttribute("aria-expanded", "true");
	}
}

// Close the kebab menu when clicking anywhere outside it
document.addEventListener("click", function (event) {
	var menu = document.querySelector(".kebab-menu");
	var dropdown = document.getElementById("kebabMenuDropdown");
	if (!menu || !dropdown || dropdown.classList.contains("hidden")) {
		return;
	}
	if (!menu.contains(event.target)) {
		dropdown.classList.add("hidden");
		var button = document.getElementById("btnKebabMenu");
		if (button) button.setAttribute("aria-expanded", "false");
	}
});

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #61): toggleSerialLog()/copySerialLog() used to live
//here too - see the comment further up where refreshLog()/displaySerialLog() used to be.