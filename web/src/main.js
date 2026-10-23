import { createApp } from "vue";
import App from "./App.vue";
import WidgetPage from "./components/WidgetPage.vue";
import "./style.css";

// /widget/<id> (operator screens, SPECIFICATION §9.3) is the same page with one component and no app chrome.
createApp(location.pathname.startsWith("/widget/") ? WidgetPage : App).mount("#app");
