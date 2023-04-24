export default function fetchPage(element, page) {

	if (!element) {
		return;
	}

	fetch(page.replace(/\?.*/, ""))
		.then(response => response.text())
		.then(html => element.innerHTML = html);
}
