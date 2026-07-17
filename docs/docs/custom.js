document.addEventListener("DOMContentLoaded", function () {
  initLanguageSelector();
  initScreenshotLightbox();
});

function initLanguageSelector() {
  var languages = {
    en: "English",
    de: "Deutsch",
    fr: "Français",
    it: "Italiano",
    pt: "Português",
    hu: "Magyar",
    pl: "Polski"
  };

  var pageMap = {
    index: { en: "index", de: "index-de", fr: "index-fr", it: "index-it", pt: "index-pt", hu: "index-hu", pl: "index-pl" },
    faq: { en: "faq", de: "faq-de", fr: "faq-fr", it: "faq-it", pt: "faq-pt" },
    manual: {
      en: "2.2.1/221_4_manual",
      de: "2.2.1/221_4_manual_de",
      fr: "2.2.1/221_4_manual_fr",
      it: "2.2.1/221_4_manual_it",
      pt: "2.2.1/221_4_manual_pt",
      hu: "2.2.1/221_4_manual_hu",
      pl: "2.2.1/221_4_manual_pl"
    },
    api: {
      en: "2.2.1/221_4_api",
      de: "2.2.1/221_4_api_de",
      fr: "2.2.1/221_4_api_fr",
      it: "2.2.1/221_4_api_it",
      pt: "2.2.1/221_4_api_pt"
    },
    opensprinklerpro: {
      en: "opensprinklerpro",
      de: "opensprinklerpro_de",
      fr: "opensprinklerpro_fr",
      it: "opensprinklerpro_it",
      pt: "opensprinklerpro_pt",
      hu: "opensprinklerpro_hu",
      pl: "opensprinklerpro_pl"
    },
    zigbee: {
      en: "zigbee",
      de: "zigbee-de",
      fr: "zigbee-fr",
      it: "zigbee-it",
      pt: "zigbee-pt",
      hu: "zigbee-hu",
      pl: "zigbee-pl"
    },
    "analog-sensor-config": {
      en: "analog-sensor-config",
      de: "analog-sensor-config-de",
      fr: "analog-sensor-config-fr",
      it: "analog-sensor-config-it",
      pt: "analog-sensor-config-pt"
    },
    "pro-api-endpoints": {
      en: "pro-api-endpoints",
      de: "pro-api-endpoints-de",
      fr: "pro-api-endpoints-fr",
      it: "pro-api-endpoints-it",
      pt: "pro-api-endpoints-pt"
    },
    "mcp-server": {
      en: "mcp-server",
      de: "mcp-server-de",
      fr: "mcp-server-fr",
      it: "mcp-server-it",
      pt: "mcp-server-pt"
    },
    "ai-programming": {
      en: "ai-programming",
      de: "ai-programming-de",
      fr: "ai-programming-fr",
      it: "ai-programming-it",
      pt: "ai-programming-pt"
    },
    "sensor-automation": {
      en: "sensor-automation",
      de: "sensor-automation-de",
      fr: "sensor-automation-fr",
      it: "sensor-automation-it",
      pt: "sensor-automation-pt"
    },
    "rainmaker-provisioning": {
      en: "rainmaker-provisioning",
      de: "rainmaker-provisioning-de",
      fr: "rainmaker-provisioning-fr",
      it: "rainmaker-provisioning-it",
      pt: "rainmaker-provisioning-pt"
    },
    "fyta-sensors": {
      en: "fyta-sensors",
      de: "fyta-sensors-de",
      fr: "fyta-sensors-fr",
      it: "fyta-sensors-it",
      pt: "fyta-sensors-pt"
    },
    troubleshooting: {
      en: "troubleshooting",
      de: "troubleshooting-de",
      fr: "troubleshooting-fr",
      it: "troubleshooting-it",
      pt: "troubleshooting-pt"
    },
    archive: {
      en: "archive",
      de: "archive-de",
      fr: "archive-fr",
      it: "archive-it",
      pt: "archive-pt"
    }
  };

  var labels = {
    en: {
      "Home": "Home",
      "Base Manual v2.2.1(4)": "Base Manual v2.2.1(4)",
      "User Manual": "User Manual",
      "API Reference": "API Reference",
      "OpenSprinklerPro Extensions": "OpenSprinklerPro Extensions",
      "Overview": "Overview",
      "Zigbee Configuration": "Zigbee Configuration",
      "Analog Sensor Config": "Analog Sensor Config",
      "API and platform addendum": "API and platform addendum",
      "MCP and AI": "MCP and AI",
      "MCP Server": "MCP Server",
      "AI Program Creation": "AI Program Creation",
      "Sensor Automation": "Sensor Automation",
      "RainMaker Provisioning": "RainMaker Provisioning",
      "FYTA Sensors": "FYTA Sensors",
      "Troubleshooting": "Troubleshooting",
      "Archived": "Archived",
      "Previous Manuals and API docs": "Previous Manuals and API docs",
      "FAQ": "FAQ",
      "Language": "Language"
    },
    de: {
      "Home": "Startseite",
      "Base Manual v2.2.1(4)": "Basishandbuch v2.2.1(4)",
      "User Manual": "Benutzerhandbuch",
      "API Reference": "API-Referenz",
      "OpenSprinklerPro Extensions": "OpenSprinklerPro-Erweiterungen",
      "Overview": "Übersicht",
      "Zigbee Configuration": "Zigbee-Konfiguration",
      "Analog Sensor Config": "Analoge Sensoren",
      "API and platform addendum": "API- und Plattformergänzung",
      "MCP and AI": "MCP und KI",
      "MCP Server": "MCP-Server",
      "AI Program Creation": "KI-Programmerstellung",
      "Sensor Automation": "Sensorautomatisierung",
      "RainMaker Provisioning": "RainMaker-Einrichtung",
      "FYTA Sensors": "FYTA-Sensoren",
      "Troubleshooting": "Fehlerbehebung",
      "Archived": "Archiv",
      "Previous Manuals and API docs": "Frühere Handbücher und API-Dokumente",
      "FAQ": "FAQ",
      "Language": "Sprache"
    },
    fr: {
      "Home": "Accueil",
      "Base Manual v2.2.1(4)": "Manuel de base v2.2.1(4)",
      "User Manual": "Manuel utilisateur",
      "API Reference": "Référence API",
      "OpenSprinklerPro Extensions": "Extensions OpenSprinklerPro",
      "Overview": "Présentation",
      "Zigbee Configuration": "Configuration de Zigbee",
      "Analog Sensor Config": "Capteurs analogiques",
      "API and platform addendum": "Complément API et plateformes",
      "MCP and AI": "MCP et IA",
      "MCP Server": "Serveur MCP",
      "AI Program Creation": "Création de programmes par IA",
      "Sensor Automation": "Automatisation des capteurs",
      "RainMaker Provisioning": "Provisionnement RainMaker",
      "FYTA Sensors": "Capteurs FYTA",
      "Troubleshooting": "Dépannage",
      "Archived": "Archives",
      "Previous Manuals and API docs": "Anciens manuels et docs API",
      "FAQ": "FAQ",
      "Language": "Langue"
    },
    it: {
      "Home": "Home",
      "Base Manual v2.2.1(4)": "Manuale base v2.2.1(4)",
      "User Manual": "Manuale utente",
      "API Reference": "Riferimento API",
      "OpenSprinklerPro Extensions": "Estensioni OpenSprinklerPro",
      "Overview": "Panoramica",
      "Zigbee Configuration": "Configurazione Zigbee",
      "Analog Sensor Config": "Sensori analogici",
      "API and platform addendum": "Appendice API e piattaforme",
      "MCP and AI": "MCP e IA",
      "MCP Server": "Server MCP",
      "AI Program Creation": "Creazione programmi con IA",
      "Sensor Automation": "Automazione sensori",
      "RainMaker Provisioning": "Provisioning RainMaker",
      "FYTA Sensors": "Sensori FYTA",
      "Troubleshooting": "Risoluzione problemi",
      "Archived": "Archivio",
      "Previous Manuals and API docs": "Manuali e documenti API precedenti",
      "FAQ": "FAQ",
      "Language": "Lingua"
    },
    pt: {
      "Home": "Início",
      "Base Manual v2.2.1(4)": "Manual de base v2.2.1(4)",
      "User Manual": "Manual do usuário",
      "API Reference": "Referência da API",
      "OpenSprinklerPro Extensions": "Extensões OpenSprinklerPro",
      "Overview": "Visão geral",
      "Zigbee Configuration": "Configuração do Zigbee",
      "Analog Sensor Config": "Configuração de sensor analógico",
      "API and platform addendum": "Adendo de API e plataforma",
      "MCP and AI": "MCP e IA",
      "MCP Server": "Servidor MCP",
      "AI Program Creation": "Criação de programa por IA",
      "Sensor Automation": "Automação do sensor",
      "RainMaker Provisioning": "Provisionamento RainMaker",
      "FYTA Sensors": "Sensores FYTA",
      "Troubleshooting": "Solução de problemas",
      "Archived": "Arquivado",
      "Previous Manuals and API docs": "Manuais e documentos de API anteriores",
      "FAQ": "FAQ",
      "Language": "Idioma"
    },
    hu: {
      "Home": "Kezdőlap",
      "Base Manual v2.2.1(4)": "Alap kézikönyv v2.2.1(4)",
      "User Manual": "Felhasználói kézikönyv",
      "API Reference": "API referencia",
      "OpenSprinklerPro Extensions": "OpenSprinklerPro kiterjesztések",
      "Overview": "Áttekintés",
      "Zigbee Configuration": "Zigbee konfiguráció",
      "Analog Sensor Config": "Analóg érzékelő konfiguráció",
      "API and platform addendum": "API és platform kiegészítés",
      "MCP and AI": "MCP és AI",
      "MCP Server": "MCP szerver",
      "AI Program Creation": "AI programlétrehozás",
      "Sensor Automation": "Érzékelő automatizáció",
      "RainMaker Provisioning": "RainMaker konfiguráció",
      "FYTA Sensors": "FYTA érzékelők",
      "Troubleshooting": "Hibaelhárítás",
      "Archived": "Archivált",
      "Previous Manuals and API docs": "Korábbi kézikönyvek és API dokumentáció",
      "FAQ": "GYIK",
      "Language": "Nyelv"
    },
    pl: {
      "Home": "Strona główna",
      "Base Manual v2.2.1(4)": "Instrukcja bazowa v2.2.1(4)",
      "User Manual": "Instrukcja obsługi",
      "API Reference": "Dokumentacja API",
      "OpenSprinklerPro Extensions": "Rozszerzenia OpenSprinklerPro",
      "Overview": "Przegląd",
      "Zigbee Configuration": "Konfiguracja Zigbee",
      "Analog Sensor Config": "Konfiguracja czujników analogowych",
      "API and platform addendum": "Aneks do API i platformy",
      "MCP and AI": "MCP i AI",
      "MCP Server": "Serwer MCP",
      "AI Program Creation": "Tworzenie programów AI",
      "Sensor Automation": "Automatyzacja czujników",
      "RainMaker Provisioning": "Konfiguracja RainMaker",
      "FYTA Sensors": "Czujniki FYTA",
      "Troubleshooting": "Rozwiązywanie problemów",
      "Archived": "Archiwum",
      "Previous Manuals and API docs": "Poprzednie instrukcje i API",
      "FAQ": "Często zadawane pytania",
      "Language": "Język"
    }
  };

  var current = findCurrentPage(pageMap);
  var storedLanguage = localStorage.getItem("os-docs-language");
  var browserLanguage = getBrowserLanguage(languages);
  var selectedLanguage = getSelectedLanguage(current, storedLanguage, browserLanguage, languages);
  if (!languages[selectedLanguage]) {
    selectedLanguage = "en";
  }

  if (current.key && pageMap[current.key][selectedLanguage] && pageMap[current.key][selectedLanguage] !== current.path) {
    window.location.replace(buildPageUrl(current.prefix, pageMap[current.key][selectedLanguage], window.location.search + window.location.hash));
    return;
  }

  insertLanguageSelector(languages, labels, selectedLanguage, function (language) {
    localStorage.setItem("os-docs-language", language);
    var active = findCurrentPage(pageMap);
    if (active.key && pageMap[active.key][language] && pageMap[active.key][language] !== active.path) {
      window.location.href = buildPageUrl(active.prefix, pageMap[active.key][language], window.location.search + window.location.hash);
      return;
    }
    updateNavigation(pageMap, labels, language, active.prefix);
    updateCurrentPageHeadingLinks(pageMap, active, language);
    updateLanguageSelectorLabel(labels, language);
  });

  updateNavigation(pageMap, labels, selectedLanguage, current.prefix);
  updateCurrentPageHeadingLinks(pageMap, current, selectedLanguage);
  updateLanguageSelectorLabel(labels, selectedLanguage);
}

function getSelectedLanguage(current, storedLanguage, browserLanguage, languages) {
  if (current.key && current.language && current.language !== "en") {
    return current.language;
  }

  if (storedLanguage && languages[storedLanguage]) {
    return storedLanguage;
  }

  if (browserLanguage && languages[browserLanguage]) {
    return browserLanguage;
  }

  return current.language || "en";
}

function getBrowserLanguage(languages) {
  var browserLanguages = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || navigator.userLanguage];

  for (var i = 0; i < browserLanguages.length; i += 1) {
    var language = (browserLanguages[i] || "").toLowerCase().split("-")[0];
    if (languages[language]) {
      return language;
    }
  }

  return "en";
}

function normalizePath(pathname) {
  var path = pathname.replace(/\/index\.html$/, "/").replace(/\.html$/, "");
  path = path.replace(/^\/+|\/+$/g, "");
  return path || "index";
}

function findCurrentPage(pageMap) {
  var normalized = normalizePath(window.location.pathname);
  for (var key in pageMap) {
    for (var language in pageMap[key]) {
      var target = pageMap[key][language];
      if (normalized === target || normalized.endsWith("/" + target)) {
        var index = normalized.lastIndexOf(target);
        var prefix = normalized.slice(0, index).replace(/\/+$/, "");
        return {
          key: key,
          language: language,
          path: target,
          prefix: prefix ? "/" + prefix + "/" : "/"
        };
      }
    }
  }
  return { key: null, language: "en", path: normalized, prefix: "/" };
}

function buildPageUrl(prefix, pagePath, suffix) {
  var cleanPrefix = prefix || "/";
  var cleanSuffix = suffix || "";
  if (!cleanPrefix.endsWith("/")) {
    cleanPrefix += "/";
  }
  if (pagePath === "index") {
    return cleanPrefix + cleanSuffix;
  }
  return cleanPrefix + pagePath + "/" + cleanSuffix;
}

function insertLanguageSelector(languages, labels, selectedLanguage, onChange) {
  var container = document.createElement("div");
  container.className = "language-selector";

  var label = document.createElement("label");
  label.setAttribute("for", "language-selector-combobox");
  label.setAttribute("data-original-label", "Language");
  label.textContent = (labels[selectedLanguage] && labels[selectedLanguage].Language) || "Language";

  var select = document.createElement("select");
  select.id = "language-selector-combobox";
  select.setAttribute("aria-label", "Select documentation language");

  Object.keys(languages).forEach(function (language) {
    var option = document.createElement("option");
    option.value = language;
    option.textContent = languages[language];
    option.selected = language === selectedLanguage;
    select.appendChild(option);
  });

  select.addEventListener("change", function () {
    onChange(select.value);
  });

  container.appendChild(label);
  container.appendChild(select);

  var content = document.querySelector(".rst-content") || document.querySelector(".wy-nav-content");
  if (content) {
    content.insertBefore(container, content.firstChild);
  }
}

function updateLanguageSelectorLabel(labels, language) {
  var label = document.querySelector('.language-selector label[for="language-selector-combobox"]');
  if (label) {
    label.textContent = (labels[language] && labels[language].Language) || "Language";
  }
}

function updateNavigation(pageMap, labels, language, prefix) {
  var navLabels = labels[language] || labels.en;
  var links = document.querySelectorAll(".wy-menu-vertical a");
  var sectionLabels = document.querySelectorAll(".wy-menu-vertical .caption-text, .wy-menu-vertical p.caption");

  sectionLabels.forEach(function (label) {
    var originalText = label.getAttribute("data-original-label") || label.textContent.trim();
    if (!label.getAttribute("data-original-label")) {
      label.setAttribute("data-original-label", originalText);
    }

    if (navLabels[originalText]) {
      label.textContent = navLabels[originalText];
    }
  });

  links.forEach(function (link) {
    var originalText = link.getAttribute("data-original-label") || link.textContent.trim();
    if (!link.getAttribute("data-original-label")) {
      link.setAttribute("data-original-label", originalText);
    }

    if (navLabels[originalText]) {
      link.textContent = navLabels[originalText];
    }

    var targetKey = getPageKeyFromHref(link.getAttribute("href"), pageMap);
    if (targetKey && pageMap[targetKey][language]) {
      link.setAttribute("href", buildPageUrl(prefix, pageMap[targetKey][language]));
    }
  });
}

function updateCurrentPageHeadingLinks(pageMap, active, language) {
  document.querySelectorAll("ul[data-generated-heading-links]").forEach(function (list) {
    list.remove();
  });

  if (!active.key || !pageMap[active.key] || !pageMap[active.key][language]) {
    return;
  }

  var navLink = findNavigationLinkForPage(pageMap, active.key);
  if (!navLink || !navLink.parentElement) {
    return;
  }

  var navItem = navLink.parentElement;
  navItem.classList.add("current");
  navLink.classList.add("current");

  if (navItem.querySelector("ul.current:not([data-generated-heading-links])")) {
    return;
  }

  var headings = Array.prototype.slice.call(document.querySelectorAll(".rst-content .document h2[id], .rst-content .document h3[id]"))
    .filter(function (heading) {
      return heading.id && getHeadingText(heading);
    });

  if (!headings.length) {
    return;
  }

  var list = document.createElement("ul");
  list.className = "current";
  list.setAttribute("data-generated-heading-links", "true");

  headings.forEach(function (heading) {
    var item = document.createElement("li");
    item.className = heading.tagName.toLowerCase() === "h3" ? "toctree-l3" : "toctree-l2";

    var link = document.createElement("a");
    link.className = "reference internal";
    link.href = "#" + heading.id;
    link.textContent = getHeadingText(heading);

    item.appendChild(link);
    list.appendChild(item);
  });

  navItem.appendChild(list);
}

function findNavigationLinkForPage(pageMap, pageKey) {
  var links = document.querySelectorAll(".wy-menu-vertical a");
  for (var i = 0; i < links.length; i += 1) {
    if (getPageKeyFromHref(links[i].getAttribute("href"), pageMap) === pageKey) {
      return links[i];
    }
  }
  return null;
}

function getHeadingText(heading) {
  var clone = heading.cloneNode(true);
  clone.querySelectorAll(".headerlink").forEach(function (link) {
    link.remove();
  });
  return clone.textContent.replace(/\s+/g, " ").trim();
}

function getPageKeyFromHref(href, pageMap) {
  if (!href || href.indexOf("#") === 0) {
    return null;
  }

  var anchor = document.createElement("a");
  anchor.href = href;
  var normalized = normalizePath(anchor.pathname);

  for (var key in pageMap) {
    for (var language in pageMap[key]) {
      var target = pageMap[key][language];
      if (normalized === target || normalized.endsWith("/" + target)) {
        return key;
      }
    }
  }
  return null;
}

function initScreenshotLightbox() {
  var screenshots = document.querySelectorAll("img.mobile-screenshot");
  if (!screenshots.length) {
    return;
  }

  var lightbox = document.createElement("div");
  lightbox.className = "screenshot-lightbox";
  lightbox.setAttribute("role", "dialog");
  lightbox.setAttribute("aria-modal", "true");
  lightbox.innerHTML = '<span class="screenshot-lightbox__close" aria-hidden="true">&times;</span><img alt="">';
  document.body.appendChild(lightbox);

  var fullImage = lightbox.querySelector("img");

  function closeLightbox() {
    lightbox.classList.remove("is-open");
    fullImage.removeAttribute("src");
    fullImage.removeAttribute("alt");
  }

  screenshots.forEach(function (image) {
    image.setAttribute("tabindex", "0");
    image.setAttribute("title", "Open full-size screenshot");

    function openLightbox() {
      fullImage.src = image.currentSrc || image.src;
      fullImage.alt = image.alt || "";
      lightbox.classList.add("is-open");
    }

    image.addEventListener("click", openLightbox);
    image.addEventListener("keydown", function (event) {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        openLightbox();
      }
    });
  });

  lightbox.addEventListener("click", closeLightbox);
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape" && lightbox.classList.contains("is-open")) {
      closeLightbox();
    }
  });
}
