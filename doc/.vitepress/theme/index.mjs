import { h } from "vue"
import DefaultTheme from "vitepress/theme"
import { VPHomeHero, VPHomeFeatures } from "vitepress/theme"
import "./home.css"

// The default theme hides the sidebar on any page with `layout: home`, so the
// home page uses the ordinary doc layout and borrows the hero and the feature
// cards from the home layout. Both components render nothing unless the page
// has `hero` or `features` in its frontmatter, so every other page is
// unaffected.
export default {
	extends: DefaultTheme,
	Layout() {
		return h(DefaultTheme.Layout, null, {
			"doc-top": () => [h(VPHomeHero), h(VPHomeFeatures)]
		})
	}
}
