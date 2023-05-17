export default function fetchPage(element, page) {

	fetch(page.replace(/\?.*/, ""))
		.then(response => response.text())
		.then(html => element.innerHTML = html);
}
