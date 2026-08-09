//Used for local development use
const localDevelopment = false;

//Various variables
var unitCount = 0;
var timezoneOffset = 0;
var fullDebugLogData = null; // Store full debug log for copying
var fullSerialLogData = null; // Store full serial log for copying
var isPageLoading = true; // Track if page is still loading
var pageLoadErrors = []; // Store page load errors and status messages

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
	}
}

// Log page load errors and status
function logPageLoadStatus(message, isError) {
	var timestamp = new Date().toLocaleTimeString();
	var logEntry = "[" + timestamp + "] " + message;
	pageLoadErrors.push(logEntry);
	
	var container = document.getElementById("pageLoadErrorContent");
	if (container) {
		var color = isError ? "#f48771" : "#4ec9b0";
		var html = container.innerHTML;
		html += '<div style="margin-bottom: 2px; padding: 1px 0; color: ' + color + ';">' + escapeHtml(logEntry) + '</div>';
		container.innerHTML = html;
		container.scrollTop = container.scrollHeight;
		
		// Show the error log if there are errors
		var errorLogDiv = document.getElementById("pageLoadErrorLog");
		if (errorLogDiv && (isError || pageLoadErrors.length > 0)) {
			errorLogDiv.style.display = "block";
		}
	}
	
	// Also log to console
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

// Start loading debug log immediately when page loads
var debugLogInterval = null;
window.addEventListener('DOMContentLoaded', function() {
	logPageLoadStatus("DOMContentLoaded event fired", false);
	loadDebugLog();
	debugLogInterval = setInterval(loadDebugLog, 500); // Refresh every 500ms during page load
});

// Request and retrieve settings from ESP-01s filesystem
function loadPage() {
	//Show messages from the server if need be
	const urlParams = new URLSearchParams(location.search);
	if (urlParams.get('invalid-submission') === "true") {
		showBannerMessage(`
			Something went wrong during submission. Feel free to try again, ensure that you have entered valid information.
			<br>
			Ensure things like dates provided for schedules/countdowns are in the future.
		`);
	}
	else if (urlParams.get('is-resetting-units') === "true") {
		showBannerMessage(`
			Display is now resetting/re-calibrating. It should only take a few seconds.
			<br>
			It will display different characters in order to carry this out and then go back to the last thing being displayed.
		`);
	}
	
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
		setVersion("Development")
		setUnitCount("10");
		setLastReceivedMessage(new Date().toLocaleString());
		setWiFiStatus("Connected", -65, "192.168.1.100"); // Mock WiFi status for development
		setCountdownDate((Date.now() / 1000) + (24 * 60 * 60));
		showHideResetWifiSettingsAction(false);
		showHideOtaUpdateAction(false);
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
						setVersion(responseObject.version);
						setUnitCount(responseObject.unitCount);
						setCountdownDate(responseObject.countdownToDateUnix);
						setLastReceivedMessage(responseObject.lastTimeReceivedMessageDateTime);
						setWiFiStatus(responseObject.wifiStatus, responseObject.wifiRssi, responseObject.wifiIp);
						showHideResetWifiSettingsAction(responseObject.wifiSettingsResettable);
						showHideOtaUpdateAction(responseObject.otaEnabled);
						
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
	labelLineCount.innerHTML = Math.ceil(length / unitCount) + inputText.split("\\n").length - 1;
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
function setVersion(version) {
	document.getElementById("labelVersion").innerHTML = version;
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

function showHideResetWifiSettingsAction(isWifiApMode) {
	if (!isWifiApMode) {
		var linkActionResetWifi = document.getElementById("linkActionResetWifi");
		linkActionResetWifi.classList.add("hidden");
	}
}

function showHideOtaUpdateAction(isOtaEnabled) {
	if (!isOtaEnabled) {
		var linkActionOtaUpdate = document.getElementById("linkActionOtaUpdate");
		linkActionOtaUpdate.classList.add("hidden");
	}
}

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
		var message = `<b>Message:</b> ${scheduledMessage.message.trim() == "" ? "<Blank>" : scheduledMessage.message}` 
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
		actionButtonElement.setAttribute('onclick', `deleteScheduledMessage(${scheduledMessage.scheduledDateTimeUnix}, '${scheduledMessage.message}')`);
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
	
	// Stop the fast debug log refresh, switch to slower refresh
	if (debugLogInterval) {
		clearInterval(debugLogInterval);
		debugLogInterval = null;
	}
	
	isPageLoading = false; // Page has finished loading
	
	// Load serial log
	refreshLog();
	// Auto-refresh log every 2 seconds
	setInterval(refreshLog, 2000);
}

// Fetch and display serial log
function refreshLog() {
	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4 && this.status == 200) {
			var responseObject = JSON.parse(this.responseText);
			displaySerialLog(responseObject);
		}
	};

	xhrRequest.open("GET", "/log", true);
	xhrRequest.send();
}

// Display serial log messages
function displaySerialLog(logData) {
	var container = document.getElementById("containerSerialLog");
	var countElement = document.getElementById("spanLogCount");
	
	// Store full log data for copying
	fullSerialLogData = logData;
	
	countElement.innerText = logData.count || 0;
	
	if (!logData.logs || logData.logs.length === 0) {
		container.innerHTML = '<div style="color: #888;">No log messages yet.</div>';
		return;
	}
	
	var html = "";
	for (var i = 0; i < logData.logs.length; i++) {
		var logEntry = logData.logs[i];
		// Timestamp is in milliseconds since boot, format as seconds with 1 decimal
		var timestampSec = (logEntry.timestamp / 1000).toFixed(1) + "s";
		var message = logEntry.message || "";
		
		// Escape HTML and add some color coding
		message = escapeHtml(message);
		if (message.indexOf("DEBUG:") >= 0) {
			message = '<span style="color: #4ec9b0;">' + message + '</span>';
		} else if (message.indexOf("ERROR") >= 0 || message.indexOf("Error") >= 0) {
			message = '<span style="color: #f48771;">' + message + '</span>';
		} else if (message.indexOf("WARNING") >= 0 || message.indexOf("Warning") >= 0) {
			message = '<span style="color: #dcdcaa;">' + message + '</span>';
		}
		
		html += '<div style="margin-bottom: 4px; padding: 2px 0; border-bottom: 1px solid #333;">';
		html += '<span style="color: #808080; margin-right: 10px;">[' + timestampSec + ']</span>';
		html += message;
		html += '</div>';
	}
	
	container.innerHTML = html;
	// Auto-scroll to bottom
	container.scrollTop = container.scrollHeight;
}

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

// Load and display debug log (for page load debugging)
function loadDebugLog() {
	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					displayDebugLog(responseObject);
				} catch (e) {
					logPageLoadStatus("ERROR: Failed to parse /log response: " + e.message, true);
				}
			} else {
				logPageLoadStatus("ERROR: /log request failed with status " + this.status, true);
			}
		}
	};
	
	xhrRequest.onerror = function() {
		logPageLoadStatus("ERROR: Network error on /log request", true);
	};

	xhrRequest.open("GET", "/log", true);
	xhrRequest.send();
}

// Display debug log in the top viewer
function displayDebugLog(logData) {
	var container = document.getElementById("debugLogContent");
	
	// Store full log data for copying
	fullDebugLogData = logData;
	
	if (!logData.logs || logData.logs.length === 0) {
		container.innerHTML = '<div style="color: #888;">No log messages yet.</div>';
		return;
	}
	
	// During page load, show full log. After page loads, show only last 15 messages
	var startIdx = isPageLoading ? 0 : Math.max(0, logData.logs.length - 15);
	var html = "";
	for (var i = startIdx; i < logData.logs.length; i++) {
		var logEntry = logData.logs[i];
		var timestampSec = (logEntry.timestamp / 1000).toFixed(1) + "s";
		var message = logEntry.message || "";
		
		// Escape HTML and add color coding
		message = escapeHtml(message);
		var color = "#d4d4d4"; // default
		if (message.indexOf("DEBUG:") >= 0) {
			color = "#4ec9b0";
		} else if (message.indexOf("ERROR") >= 0 || message.indexOf("Error") >= 0) {
			color = "#f48771";
		} else if (message.indexOf("WARNING") >= 0 || message.indexOf("Warning") >= 0) {
			color = "#dcdcaa";
		}
		
		html += '<div style="margin-bottom: 2px; padding: 1px 0; font-size: 0.9em;">';
		html += '<span style="color: #808080; margin-right: 8px;">[' + timestampSec + ']</span>';
		html += '<span style="color: ' + color + ';">' + message + '</span>';
		html += '</div>';
	}
	
	container.innerHTML = html;
	// Auto-scroll to bottom
	container.scrollTop = container.scrollHeight;
}

// Toggle debug log viewer visibility
function toggleDebugLog() {
	var viewer = document.getElementById("debugLogViewer");
	if (viewer.style.display === "none") {
		viewer.style.display = "block";
		loadDebugLog();
	} else {
		viewer.style.display = "none";
	}
}

// Toggle page load error log visibility
function togglePageLoadLog() {
	var viewer = document.getElementById("pageLoadErrorLog");
	if (viewer.style.display === "none") {
		viewer.style.display = "block";
	} else {
		viewer.style.display = "none";
	}
}

// Copy full debug log to clipboard
function copyDebugLog() {
	if (!fullDebugLogData || !fullDebugLogData.logs || fullDebugLogData.logs.length === 0) {
		alert("No log data available to copy.");
		return;
	}
	
	var logText = "";
	for (var i = 0; i < fullDebugLogData.logs.length; i++) {
		var logEntry = fullDebugLogData.logs[i];
		var timestampSec = (logEntry.timestamp / 1000).toFixed(1) + "s";
		var message = logEntry.message || "";
		logText += "[" + timestampSec + "] " + message + "\n";
	}
	
	// Copy to clipboard
	var textArea = document.createElement("textarea");
	textArea.value = logText;
	textArea.style.position = "fixed";
	textArea.style.opacity = "0";
	document.body.appendChild(textArea);
	textArea.select();
	try {
		document.execCommand('copy');
		alert("Debug log copied to clipboard!");
	} catch (err) {
		alert("Failed to copy log. Please select and copy manually.");
	}
	document.body.removeChild(textArea);
}

// Toggle serial log visibility
function toggleSerialLog() {
	var container = document.getElementById("containerSerialLog");
	var button = document.getElementById("btnToggleSerialLog");
	
	if (container.style.display === "none") {
		container.style.display = "block";
		if (button) button.textContent = "Hide";
		refreshLog(); // Refresh when showing
	} else {
		container.style.display = "none";
		if (button) button.textContent = "Show";
	}
}

// Copy full serial log to clipboard
function copySerialLog() {
	if (!fullSerialLogData || !fullSerialLogData.logs || fullSerialLogData.logs.length === 0) {
		alert("No log data available to copy.");
		return;
	}
	
	var logText = "";
	for (var i = 0; i < fullSerialLogData.logs.length; i++) {
		var logEntry = fullSerialLogData.logs[i];
		var timestampSec = (logEntry.timestamp / 1000).toFixed(1) + "s";
		var message = logEntry.message || "";
		logText += "[" + timestampSec + "] " + message + "\n";
	}
	
	// Copy to clipboard
	var textArea = document.createElement("textarea");
	textArea.value = logText;
	textArea.style.position = "fixed";
	textArea.style.opacity = "0";
	document.body.appendChild(textArea);
	textArea.select();
	try {
		document.execCommand('copy');
		alert("Serial log copied to clipboard!");
	} catch (err) {
		alert("Failed to copy log. Please select and copy manually.");
	}
	document.body.removeChild(textArea);
}