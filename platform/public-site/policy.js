const notice = document.querySelector("#sent");
if (notice instanceof HTMLElement && new URLSearchParams(window.location.search).get("sent") === "1") {
  notice.hidden = false;
}
