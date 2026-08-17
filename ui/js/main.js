import "uikit/dist/css/uikit.min.css";
import "~/css/app.css";

import { BindingRoot } from "@datumjs/datum";
import DatabaseClient from "./DatabaseClient";
import fetchPage from "./fetchPage";
import App from "~/js/App";

new BindingRoot(new App(new DatabaseClient(), fetchPage));
