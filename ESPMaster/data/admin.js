//Used for local development use, mirrors the flag in script.js
const localDevelopment = false;

var unitsData = [];
//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): true while ANY unit is recalibrating after an
//offset change - disables every "Save & Recalibrate" button (not just the one being edited)
//while true, so the UI itself can't trigger overlapping I2C operations. Doesn't fix the
//underlying Wire.requestFrom() hang risk (a long-standing, deliberately-not-touched-again issue,
//see BUGFIX_LOG.md's "compare upstream" lesson and the reverted fix in #40) - just avoids
//provoking it from here. Became noticeably more likely to trigger with 3 units instead of 1
//(more chances for a status poll to land while a different unit is mid-recalibration).
var recalibrationInProgress = false;
//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #61): moved here along with the Serial Log card itself -
//see refreshLog()/toggleSerialLog()/copySerialLog() below.
var fullSerialLogData = null; // Store full serial log for copying
var serialLogInterval = null; // Holds the /log auto-refresh setInterval's id so it can be stopped

window.addEventListener('load', function () {
	loadUnits();
	loadSystemActionVisibility();
	loadCurrentOtaPassword();
});

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #57): OTA Update / Reset WiFi moved here from index.html's
//old "Actions" card, along with the show/hide logic that used to live in script.js
//(showHideOtaUpdateAction()/showHideResetWifiSettingsAction()) - only relevant here now, since
//these buttons no longer exist on the main page. Reads the same /settings fields
//(otaEnabled/wifiSettingsResettable) the main page's initial load already used.
function loadSystemActionVisibility() {
	if (localDevelopment) {
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4 && this.status == 200) {
			try {
				var responseObject = JSON.parse(this.responseText);

				var formOtaUpdate = document.getElementById("formOtaUpdate");
				if (formOtaUpdate && !responseObject.otaEnabled) {
					formOtaUpdate.classList.add("hidden");
				}

				var formResetWifi = document.getElementById("formResetWifi");
				if (formResetWifi && !responseObject.wifiSettingsResettable) {
					formResetWifi.classList.add("hidden");
				}
			} catch (e) {
				// Non-critical - worst case both buttons stay visible even if not applicable.
			}
		}
	};
	xhrRequest.open("GET", "/settings", true);
	xhrRequest.send();
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #67): these three used to be real <form method="post">
//submissions - the response was a full standalone HTML page meant for classic browser
//navigation, so clicking any of them replaced the entire Administration page. On user request
//("weniger neu laden, mehr auf der Seite bleiben"), converted to AJAX with the same confirm()
//dialogs as before - the page itself never navigates away now, a banner message shows the result
//inline instead.
function triggerOtaMode() {
	if (!confirm("Are you sure you want to put device in OTA mode?")) {
		return;
	}
	postActionForBanner("/ota", "OTA mode started. Open your Arduino IDE, select the new network port, and upload as normal.");
}

function triggerResetWifi() {
	if (!confirm("Are you sure you want to Reset WiFi settings?")) {
		return;
	}
	postActionForBanner("/reset-wifi", "WiFi settings erased. The device is rebooting into AP setup mode now.");
}

function triggerResetUnits() {
	if (!confirm("Are you sure you want to Reset Units?")) {
		return;
	}
	postActionForBanner("/reset-units", "Units are resetting/recalibrating now - this only takes a few seconds.");
}

//Shared helper for the three simple fire-and-confirm admin actions above - POSTs with no body,
//shows successMessage in the banner on success, or the server's error text on failure.
function postActionForBanner(path, successMessage) {
	if (localDevelopment) {
		showBannerMessage(successMessage, true);
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				showBannerMessage(successMessage, true);
			} else {
				showBannerMessage("Failed: " + (this.responseText || ("Status " + this.status)));
			}
		}
	};
	xhrRequest.onerror = function () {
		showBannerMessage("Network error.");
	};
	xhrRequest.open("POST", path, true);
	xhrRequest.send();
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #63): fetches and displays the currently-set OTA
//password - freely, no authentication needed to view (on user request).
function loadCurrentOtaPassword() {
	var displayEl = document.getElementById("currentOtaPasswordDisplay");

	if (localDevelopment) {
		displayEl.innerText = "splitflap (simuliert)";
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					displayEl.innerText = responseObject.otaPassword || "(leer)";
				} catch (e) {
					displayEl.innerText = "(konnte nicht geladen werden)";
				}
			} else {
				displayEl.innerText = "(konnte nicht geladen werden)";
			}
		}
	};
	xhrRequest.open("GET", "/admin/ota-password", true);
	xhrRequest.send();
}

//Changes the OTA password - requires the current password (read from the same "OTA-Passwort"
//field used elsewhere on this page) for confirmation, same as every other admin action here.
function changeOtaPassword() {
	var currentPasswordInput = document.getElementById("firmwarePassword");
	var newPasswordInput = document.getElementById("newOtaPassword");
	var statusEl = document.getElementById("otaPasswordChangeStatus");

	var newPassword = newPasswordInput.value;
	if (!newPassword) {
		alert("Bitte ein neues Passwort eingeben.");
		return;
	}

	if (localDevelopment) {
		statusEl.innerHTML = '<span class="status-online-ready">Passwort geändert (simuliert).</span>';
		document.getElementById("currentOtaPasswordDisplay").innerText = newPassword;
		newPasswordInput.value = "";
		return;
	}

	statusEl.innerHTML = "";

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				statusEl.innerHTML = '<span class="status-online-ready">Passwort geändert.</span>';
				newPasswordInput.value = "";
				loadCurrentOtaPassword();
			} else if (this.status == 401) {
				statusEl.innerHTML = '<span class="status-offline">Falsches aktuelles Passwort - siehe Feld "OTA-Passwort" oben.</span>';
			} else {
				statusEl.innerHTML = '<span class="status-offline">Fehlgeschlagen: ' + escapeHtml(this.responseText || ("Status " + this.status)) + '</span>';
			}
		}
	};

	xhrRequest.onerror = function () {
		statusEl.innerHTML = '<span class="status-offline">Netzwerkfehler.</span>';
	};

	xhrRequest.open("POST", "/admin/set-ota-password", true);
	xhrRequest.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	xhrRequest.send("currentPassword=" + encodeURIComponent(currentPasswordInput.value) + "&newPassword=" + encodeURIComponent(newPassword));
}

//Re-runs the I2C scan on the ESP to pick up units connected/fixed after boot, no reboot needed
function rescanUnits() {
	if (localDevelopment) {
		loadUnits();
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					showBannerMessage("Rescan abgeschlossen: " + responseObject.unitsAmount + " Unit(s) gefunden.", true);
				} catch (e) {
					showBannerMessage("Rescan-Antwort konnte nicht gelesen werden.");
				}
			} else {
				showBannerMessage("Rescan fehlgeschlagen. Status: " + this.status);
			}
			loadUnits();
		}
	};

	xhrRequest.onerror = function () {
		showBannerMessage("Netzwerkfehler beim Rescan.");
		loadUnits();
	};

	xhrRequest.open("POST", "/admin/rescan-units", true);
	xhrRequest.send();
}

//Fetches live status + offset for every unit from the ESP
function loadUnits() {
	if (localDevelopment) {
		unitsData = {
			units: [
				{ index: 0, online: true, status: 0, offset: 103 },
				{ index: 1, online: true, status: 0, offset: 97 },
				{ index: 2, online: false, status: -2, offset: -1 },
			]
		};
		renderUnits(unitsData);
		showContent();
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					renderUnits(responseObject);
				} catch (e) {
					showBannerMessage("Failed to read unit data: " + e.message);
				}
			} else {
				showBannerMessage("Failed to load unit data. Status: " + this.status);
			}
			showContent();
		}
	};

	xhrRequest.onerror = function () {
		showBannerMessage("Network error while loading unit data.");
		showContent();
	};

	xhrRequest.open("GET", "/admin/units", true);
	xhrRequest.send();
}

//Renders the list of units and their current offset/status
function renderUnits(responseObject) {
	var container = document.getElementById("containerUnits");
	container.innerHTML = "";

	var units = responseObject.units || [];
	for (var i = 0; i < units.length; i++) {
		var unit = units[i];

		var row = document.createElement("div");
		row.className = "unit-row";
		row.id = "unitRow" + unit.index;

		var labelElement = document.createElement("div");
		labelElement.className = "unit-label";
		labelElement.innerText = "Unit " + unit.index;

		var statusElement = document.createElement("div");
		statusElement.className = "unit-status";
		statusElement.innerHTML = formatStatus(unit);

		var offsetElement = document.createElement("div");
		offsetElement.className = "unit-offset";
		offsetElement.id = "unitOffset" + unit.index;
		offsetElement.innerText = unit.online ? ("Offset: " + unit.offset) : "Offset: -";

		var inputContainer = document.createElement("div");
		inputContainer.className = "unit-input";

		var input = document.createElement("input");
		input.type = "number";
		input.min = "0";
		input.max = "2037";
		input.id = "unitInput" + unit.index;
		input.value = unit.online ? unit.offset : "";
		input.disabled = !unit.online;

		var button = document.createElement("button");
		button.id = "unitButton" + unit.index;
		button.innerText = "Save & Recalibrate";
		//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): disabled while ANY unit is recalibrating,
		//not just when this particular one is offline - see recalibrationInProgress above.
		button.disabled = !unit.online || recalibrationInProgress;
		button.onclick = (function (unitIndex) {
			return function () { saveOffset(unitIndex); };
		})(unit.index);

		inputContainer.appendChild(input);
		inputContainer.appendChild(button);

		row.appendChild(labelElement);
		row.appendChild(statusElement);
		row.appendChild(offsetElement);
		row.appendChild(inputContainer);

		container.appendChild(row);
	}

	if (units.length === 0) {
		container.innerHTML = '<div style="padding: 10px;">No units configured.</div>';
	}

	populateUnitFirmwareTargetDropdown(units);
}

//Keeps the "Ziel-Unit" dropdown on the firmware update card in sync with the known units,
//preserving the current selection across refreshes where possible
function populateUnitFirmwareTargetDropdown(units) {
	var select = document.getElementById("unitFirmwareTarget");
	if (!select) {
		return;
	}

	var previousValue = select.value;
	select.innerHTML = "";

	for (var i = 0; i < units.length; i++) {
		var option = document.createElement("option");
		option.value = units[i].index;
		option.innerText = "Unit " + units[i].index;
		select.appendChild(option);
	}

	if (previousValue !== "" && select.querySelector('option[value="' + previousValue + '"]')) {
		select.value = previousValue;
	}
}

//Turns the raw online/status fields into a readable, colour coded label
function formatStatus(unit) {
	if (!unit.online) {
		return '<span class="status-offline">Offline</span>';
	}

	if (unit.status === 1) {
		return '<span class="status-online-busy">Busy / Recalibrating</span>';
	}

	if (unit.status === 0) {
		return '<span class="status-online-ready">Ready</span>';
	}

	return '<span class="status-offline">Unknown (' + unit.status + ')</span>';
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): disables/enables every rendered unit's
//"Save & Recalibrate" button immediately, without waiting for a full renderUnits() re-render -
//see the comment on recalibrationInProgress above.
function setAllUnitButtonsDisabled(disabled) {
	var buttons = document.querySelectorAll('#containerUnits button[id^="unitButton"]');
	for (var i = 0; i < buttons.length; i++) {
		buttons[i].disabled = disabled;
	}
}

//Sends a new offset for a single unit, then polls until it has finished recalibrating
function saveOffset(unitIndex) {
	var input = document.getElementById("unitInput" + unitIndex);
	var button = document.getElementById("unitButton" + unitIndex);
	var offsetValue = parseInt(input.value, 10);

	// 2037 = UNIT_STEPS - 1 in ESPMaster.ino (28BYJ-48 stepper steps per revolution). Keep in sync.
	if (isNaN(offsetValue) || offsetValue < 0 || offsetValue > 2037) {
		alert("Please enter an offset between 0 and 2037.");
		return;
	}

	//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): shouldn't normally be reachable since every
	//button is disabled while this is true, but guard here too in case a click still slips
	//through (e.g. a stale render).
	if (recalibrationInProgress) {
		showBannerMessage("Another unit is still recalibrating - please wait for it to finish first.");
		return;
	}

	button.disabled = true;
	button.innerText = "Saving...";

	if (localDevelopment) {
		setTimeout(function () {
			button.disabled = false;
			button.innerText = "Save & Recalibrate";
			showBannerMessage("Unit " + unitIndex + " offset updated to " + offsetValue + " (simulated).", true);
		}, 1500);
		return;
	}

	var xhrRequest = new XMLHttpRequest();
	xhrRequest.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 202) {
				//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): lock every unit's button for the
				//duration of this recalibration, not just the one just saved - prevents the UI
				//itself from firing an overlapping status poll/offset-save against a different
				//unit while this one is mid-recalibration (the actual trigger for the
				//Wire.requestFrom() hang risk becoming a user-visible "network error").
				recalibrationInProgress = true;
				setAllUnitButtonsDisabled(true);
				showBannerMessage("Unit " + unitIndex + " is recalibrating with the new offset, this can take a few seconds...");
				pollUntilReady(unitIndex, button);
			} else {
				button.disabled = false;
				button.innerText = "Save & Recalibrate";
				showBannerMessage("Failed to set offset: " + (this.responseText || ("Status " + this.status)));
			}
		}
	};

	xhrRequest.onerror = function () {
		button.disabled = false;
		button.innerText = "Save & Recalibrate";
		showBannerMessage("Network error while setting offset.");
	};

	xhrRequest.open("POST", "/admin/unit-offset", true);
	xhrRequest.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	xhrRequest.send("id=" + encodeURIComponent(unitIndex) + "&offset=" + encodeURIComponent(offsetValue));
}

//Polls /admin/units until the given unit is done moving (or a timeout is hit), then refreshes the view
function pollUntilReady(unitIndex, button) {
	var attempts = 0;
	var maxAttempts = 20; // ~20 seconds, a full rotation shouldn't take longer than this

	var intervalId = setInterval(function () {
		attempts++;

		var xhrRequest = new XMLHttpRequest();
		xhrRequest.onreadystatechange = function () {
			if (this.readyState == 4 && this.status == 200) {
				try {
					var responseObject = JSON.parse(this.responseText);
					var units = responseObject.units || [];
					var unit = units[unitIndex];

					if (unit && unit.online && unit.status !== 1) {
						clearInterval(intervalId);
						//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): release the lock before
						//re-rendering, so renderUnits() re-enables every button based on each
						//unit's actual online status again.
						recalibrationInProgress = false;
						renderUnits(responseObject);
						showBannerMessage("Unit " + unitIndex + " is ready. Current offset: " + unit.offset, true);
						return;
					}
				} catch (e) {
					// ignore and try again on next poll
				}
			}

			if (attempts >= maxAttempts) {
				clearInterval(intervalId);
				//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #66): release the lock on every button,
				//not just this one - see recalibrationInProgress above.
				recalibrationInProgress = false;
				setAllUnitButtonsDisabled(false);
				button.innerText = "Save & Recalibrate";
				showBannerMessage("Timed out waiting for Unit " + unitIndex + " to finish recalibrating. Hit Refresh to check its status.");
			}
		};

		xhrRequest.open("GET", "/admin/units", true);
		xhrRequest.send();
	}, 1000);
}

//Shows a message up top of the page
function showBannerMessage(message, hideAfterDuration) {
	var bannerMessageElement = document.getElementById('bannerMessage');
	bannerMessageElement.innerHTML = message;
	bannerMessageElement.classList.remove("hidden");

	if (hideAfterDuration) {
		setTimeout(function () {
			bannerMessageElement.classList.add("hidden");
		}, 7500);
	}
}

function showContent() {
	document.getElementById("initialLoading").classList.add("hidden");
	document.getElementById("loadedContent").classList.remove("hidden");
}

//Helper to escape HTML, mirrors the one in script.js (admin.js is a standalone page)
function escapeHtml(text) {
	var map = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#039;'
	};
	return String(text).replace(/[&<>"']/g, function (m) { return map[m]; });
}

//Uploads a compiled .bin sketch to the ESP over the browser, no Arduino IDE required
function uploadFirmware() {
	var fileInput = document.getElementById("firmwareFile");
	var passwordInput = document.getElementById("firmwarePassword");
	var button = document.getElementById("firmwareUploadButton");
	var progressContainer = document.getElementById("firmwareProgressContainer");
	var progressBar = document.getElementById("firmwareProgressBar");
	var statusEl = document.getElementById("firmwareStatus");

	if (!fileInput.files || fileInput.files.length === 0) {
		alert("Bitte zuerst eine .bin Datei auswählen.");
		return;
	}

	var file = fileInput.files[0];
	if (!file.name.toLowerCase().endsWith(".bin")) {
		if (!confirm("Die ausgewählte Datei endet nicht auf .bin. Trotzdem hochladen?")) {
			return;
		}
	}

	if (!confirm("Firmware '" + file.name + "' jetzt auf das ESPMaster-Modul hochladen? Das Gerät startet danach neu.")) {
		return;
	}

	if (localDevelopment) {
		simulateFirmwareUpload();
		return;
	}

	var formData = new FormData();
	formData.append("firmware", file, file.name);

	button.disabled = true;
	progressContainer.classList.remove("hidden");
	progressBar.style.width = "0%";
	progressBar.innerText = "0%";
	statusEl.innerText = "Wird hochgeladen...";
	statusEl.className = "";

	var xhr = new XMLHttpRequest();

	xhr.upload.onprogress = function (e) {
		if (e.lengthComputable) {
			var percent = Math.round((e.loaded / e.total) * 100);
			progressBar.style.width = percent + "%";
			progressBar.innerText = percent + "%";
		}
	};

	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 200) {
				progressBar.style.width = "100%";
				progressBar.innerText = "100%";
				statusEl.innerText = "Update erfolgreich! Gerät startet jetzt neu, dies kann 10-20 Sekunden dauern...";
				statusEl.className = "status-online-ready";
				setTimeout(function () {
					window.location.href = "/";
				}, 15000);
			} else {
				statusEl.innerText = "Update fehlgeschlagen: " + (this.responseText || ("Status " + this.status));
				statusEl.className = "status-offline";
				button.disabled = false;
			}
		}
	};

	xhr.onerror = function () {
		statusEl.innerText = "Netzwerkfehler beim Hochladen.";
		statusEl.className = "status-offline";
		button.disabled = false;
	};

	xhr.open("POST", "/firmware-update", true);
	xhr.setRequestHeader("X-OTA-Password", passwordInput.value);
	xhr.send(formData);
}

//Uploads a compiled Unit.ino .bin to the ESP, to be pushed to a unit afterwards
function uploadUnitFirmware() {
	var fileInput = document.getElementById("unitFirmwareFile");
	var button = document.getElementById("unitFirmwareUploadButton");
	var statusEl = document.getElementById("unitFirmwareUploadStatus");

	if (!fileInput.files || fileInput.files.length === 0) {
		alert("Bitte zuerst eine .bin Datei auswählen.");
		return;
	}

	var file = fileInput.files[0];

	if (localDevelopment) {
		statusEl.innerText = "Firmware '" + file.name + "' hochgeladen (simuliert).";
		statusEl.className = "status-online-ready";
		return;
	}

	var formData = new FormData();
	formData.append("firmware", file, file.name);

	button.disabled = true;
	statusEl.innerText = "Wird hochgeladen...";
	statusEl.className = "";

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			button.disabled = false;
			if (this.status == 200) {
				statusEl.innerText = "Firmware '" + file.name + "' hochgeladen und bereit zum Übertragen.";
				statusEl.className = "status-online-ready";
			} else if (this.status == 401) {
				statusEl.innerText = "Upload abgelehnt: falsches OTA-Passwort (siehe Feld oben beim ESPMaster-Update).";
				statusEl.className = "status-offline";
			} else {
				statusEl.innerText = "Upload fehlgeschlagen: " + (this.responseText || ("Status " + this.status));
				statusEl.className = "status-offline";
			}
		}
	};

	xhr.onerror = function () {
		button.disabled = false;
		statusEl.innerText = "Netzwerkfehler beim Hochladen.";
		statusEl.className = "status-offline";
	};

	xhr.open("POST", "/unit-firmware-upload", true);
	//SECURITY FIX, 2026-08-13: this endpoint now requires the same OTA password as the
	//ESPMaster firmware update above (see ESPMaster.ino) - reuses that same input field.
	var passwordInput = document.getElementById("firmwarePassword");
	xhr.setRequestHeader("X-OTA-Password", passwordInput ? passwordInput.value : "");
	xhr.send(formData);
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #58): switches between updating a single selected unit
//and updating every known unit sequentially - see pushUnitFirmwareAll() below.
function setUnitFirmwareMode(mode) {
	var singlePanel = document.getElementById("unitFirmwareModeSingle");
	var allPanel = document.getElementById("unitFirmwareModeAll");
	var resultEl = document.getElementById("unitFirmwareResult");

	if (mode === "all") {
		singlePanel.classList.add("hidden");
		allPanel.classList.remove("hidden");
	} else {
		singlePanel.classList.remove("hidden");
		allPanel.classList.add("hidden");
	}
	resultEl.innerHTML = "";
}

//Pushes the previously uploaded firmware to the selected unit over I2C
function pushUnitFirmware() {
	var select = document.getElementById("unitFirmwareTarget");
	var button = document.getElementById("unitFirmwarePushButton");
	var resultEl = document.getElementById("unitFirmwareResult");

	if (!select.value) {
		alert("Keine Ziel-Unit verfügbar.");
		return;
	}

	if (!confirm("Firmware jetzt auf Unit " + select.value + " übertragen? Die Unit macht dabei eine kurze Zwangspause und startet danach neu.")) {
		return;
	}

	if (localDevelopment) {
		resultEl.innerHTML = '<p class="status-online-ready">Update erfolgreich (simuliert). 42 von 42 Seiten geschrieben und verifiziert.</p>';
		return;
	}

	button.disabled = true;
	resultEl.innerHTML = '<p>Update läuft, bitte warten - das dauert nur wenige Sekunden...</p>';

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			button.disabled = false;

			if (this.status == 401) {
				resultEl.innerHTML = '<p class="status-offline">Abgelehnt: falsches OTA-Passwort (siehe Feld oben beim ESPMaster-Update).</p>';
				return;
			}

			try {
				var responseObject = JSON.parse(this.responseText);
				renderUnitFirmwareResult(responseObject);
			} catch (e) {
				resultEl.innerHTML = '<p class="status-offline">Antwort konnte nicht gelesen werden: ' + escapeHtml(this.responseText || ("Status " + this.status)) + '</p>';
			}

			loadUnits();
		}
	};

	xhr.onerror = function () {
		button.disabled = false;
		resultEl.innerHTML = '<p class="status-offline">Netzwerkfehler während des Updates.</p>';
	};

	xhr.open("POST", "/unit-firmware-push", true);
	xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	//SECURITY FIX, 2026-08-13: same OTA password as the upload above and the ESPMaster update.
	var passwordInput = document.getElementById("firmwarePassword");
	xhr.setRequestHeader("X-OTA-Password", passwordInput ? passwordInput.value : "");
	xhr.send("id=" + encodeURIComponent(select.value));
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #58): updates every known unit one after another, reusing
//the exact same /unit-firmware-push call pushUnitFirmware() makes above - never fires more than
//one push at a time (see the warning text in admin.html on why: only one unit's I2C transaction
//should ever be in flight at once). Stops immediately on the first failure rather than continuing
//to the next unit, so a partially-updated fleet is never silently left in a mixed state without
//the operator noticing exactly where it stopped.
function pushUnitFirmwareAll() {
	var button = document.getElementById("unitFirmwarePushAllButton");
	var progressEl = document.getElementById("unitFirmwareAllProgress");
	var units = (unitsData && unitsData.units) || [];

	if (units.length === 0) {
		alert("Keine Units bekannt.");
		return;
	}

	if (!confirm("Firmware jetzt nacheinander auf alle " + units.length + " Unit(s) übertragen? Das kann je nach Anzahl mehrere Minuten dauern.")) {
		return;
	}

	var unitIndexes = units.map(function (u) { return u.index; });

	if (localDevelopment) {
		progressEl.innerHTML = "";
		for (var i = 0; i < unitIndexes.length; i++) {
			progressEl.innerHTML += '<p class="status-online-ready">Unit ' + unitIndexes[i] + ': ✓ erfolgreich (simuliert)</p>';
		}
		return;
	}

	button.disabled = true;
	progressEl.innerHTML = "";

	unitIndexes.forEach(function (unitIndex) {
		var row = document.createElement("p");
		row.id = "unitFirmwareAllRow" + unitIndex;
		row.innerText = "Unit " + unitIndex + ": wartet...";
		progressEl.appendChild(row);
	});

	pushUnitFirmwareAllStep(unitIndexes, 0, button);
}

//Processes one unit, then recurses into the next - kept sequential (not e.g. Promise.all) on
//purpose, see the comment on pushUnitFirmwareAll() above.
function pushUnitFirmwareAllStep(unitIndexes, stepIndex, button) {
	if (stepIndex >= unitIndexes.length) {
		button.disabled = false;
		loadUnits();
		return;
	}

	var unitIndex = unitIndexes[stepIndex];
	var row = document.getElementById("unitFirmwareAllRow" + unitIndex);
	row.innerText = "Unit " + unitIndex + ": Update läuft, bitte warten...";
	row.className = "status-online-busy";

	var xhr = new XMLHttpRequest();
	xhr.onreadystatechange = function () {
		if (this.readyState == 4) {
			if (this.status == 401) {
				row.innerText = "Unit " + unitIndex + ": abgelehnt - falsches OTA-Passwort.";
				row.className = "status-offline";
				button.disabled = false;
				return; //Wrong password won't work for any other unit either - stop the batch here.
			}

			var success = false;
			var message = "Antwort konnte nicht gelesen werden (Status " + this.status + ").";
			try {
				var responseObject = JSON.parse(this.responseText);
				success = !!responseObject.success;
				message = responseObject.success ? (responseObject.message || "Erfolgreich.") : (responseObject.error || "Fehlgeschlagen.");
				if (typeof responseObject.pagesWritten !== "undefined" && typeof responseObject.totalPages !== "undefined") {
					message += " (" + responseObject.pagesWritten + "/" + responseObject.totalPages + " Seiten)";
				}
			} catch (e) {
				// message already has a fallback set above
			}

			row.innerText = "Unit " + unitIndex + ": " + (success ? "✓ " : "✗ ") + message;
			row.className = success ? "status-online-ready" : "status-offline";

			if (!success) {
				button.disabled = false; //Stop the batch - see the comment on pushUnitFirmwareAll() above.
				loadUnits();
				return;
			}

			pushUnitFirmwareAllStep(unitIndexes, stepIndex + 1, button);
		}
	};

	xhr.onerror = function () {
		row.innerText = "Unit " + unitIndex + ": Netzwerkfehler.";
		row.className = "status-offline";
		button.disabled = false;
	};

	xhr.open("POST", "/unit-firmware-push", true);
	xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
	//SECURITY FIX, 2026-08-13: same OTA password as the upload above and the ESPMaster update.
	var passwordInput = document.getElementById("firmwarePassword");
	xhr.setRequestHeader("X-OTA-Password", passwordInput ? passwordInput.value : "");
	xhr.send("id=" + encodeURIComponent(unitIndex));
}

//Renders the JSON result of a firmware push in the result panel
function renderUnitFirmwareResult(result) {
	var resultEl = document.getElementById("unitFirmwareResult");
	var cssClass = result.success ? "status-online-ready" : "status-offline";
	var text = result.success ? (result.message || "Update erfolgreich.") : (result.error || "Update fehlgeschlagen.");

	var html = '<p class="' + cssClass + '">' + escapeHtml(text) + '</p>';
	if (typeof result.pagesWritten !== "undefined" && typeof result.totalPages !== "undefined") {
		html += '<p>Seiten geschrieben/verifiziert: ' + result.pagesWritten + ' von ' + result.totalPages + '</p>';
	}

	resultEl.innerHTML = html;
}

//Fakes a firmware upload for local development mode, mirrors localDevelopment handling elsewhere
function simulateFirmwareUpload() {
	var button = document.getElementById("firmwareUploadButton");
	var progressContainer = document.getElementById("firmwareProgressContainer");
	var progressBar = document.getElementById("firmwareProgressBar");
	var statusEl = document.getElementById("firmwareStatus");

	button.disabled = true;
	progressContainer.classList.remove("hidden");
	statusEl.innerText = "Wird hochgeladen (simuliert)...";
	statusEl.className = "";

	var percent = 0;
	var interval = setInterval(function () {
		percent += 20;
		progressBar.style.width = percent + "%";
		progressBar.innerText = percent + "%";

		if (percent >= 100) {
			clearInterval(interval);
			statusEl.innerText = "Update erfolgreich (simuliert).";
			statusEl.className = "status-online-ready";
			button.disabled = false;
		}
	}, 300);
}

//BUG FIX, 2026-08-15 (see BUGFIX_LOG.md #61): moved here from script.js along with the "Serial
//Log" card itself (now on the Administration page, on user request).

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

// Toggle serial log visibility - starts/stops the auto-refresh poll along with the panel itself
// (see BUGFIX_LOG.md #56 for why that matters: it used to keep polling /log every 2s even while
// hidden).
function toggleSerialLog() {
	var container = document.getElementById("containerSerialLog");
	var button = document.getElementById("btnToggleSerialLog");

	if (container.style.display === "none") {
		container.style.display = "block";
		if (button) button.textContent = "Hide";
		refreshLog(); // Refresh immediately when showing
		if (!serialLogInterval) {
			serialLogInterval = setInterval(refreshLog, 2000);
		}
	} else {
		container.style.display = "none";
		if (button) button.textContent = "Show";
		if (serialLogInterval) {
			clearInterval(serialLogInterval);
			serialLogInterval = null;
		}
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
