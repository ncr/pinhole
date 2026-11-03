"use strict";
(() => {
    const { judgments: initial, images, orders } = JSON.parse(document.getElementById("payload").textContent);
    const ids = Object.keys(images), idSet = new Set(ids), pageSize = 24;
    const el = id => document.getElementById(id);
    let judgments, queryIndex = 0, position = 0, dirty = false;
    function checked(value) {
        if (value.schema !== 1 || value.corpus_id !== initial.corpus_id || !Array.isArray(value.queries)
                || value.queries.length !== initial.queries.length) throw Error("Plik nie pasuje do tej próbki.");
        const copy = JSON.parse(JSON.stringify(value));
        copy.queries.forEach((q, index) => {
            const expected = initial.queries[index];
            if (q.id !== expected.id || q.text !== expected.text || typeof q.complete !== "boolean"
                    || q.relevance === null || typeof q.relevance !== "object" || Array.isArray(q.relevance))
                throw Error("Zapytania w pliku różnią się od tej oceny.");
            for (const [id, grade] of Object.entries(q.relevance))
                if (!idSet.has(id) || !Number.isInteger(grade) || grade < 0 || grade > 2)
                    throw Error("Plik zawiera nieprawidłową ocenę.");
            // Legacy completed judgments explicitly declare omitted IDs irrelevant.
            if (q.complete) for (const id of ids) if (!Object.hasOwn(q.relevance, id)) q.relevance[id] = 0;
        });
        return copy;
    }
    judgments = checked(initial);
    const query = () => judgments.queries[queryIndex];
    const order = () => orders[query().id];
    function message(text, error = false) { el("status").textContent = text; el("status").className = error ? "error" : ""; }
    function progress() {
        const q = query(), reviewed = Object.keys(q.relevance).length;
        el("progress").textContent = `Oceniono ${reviewed} z ${ids.length}. `
            + (q.complete ? "Zapytanie zatwierdzone." : "Zapytanie niezatwierdzone.")
            + ` Zatwierdzone zapytania: ${judgments.queries.filter(q => q.complete).length} z ${judgments.queries.length}.`;
        el("complete").disabled = reviewed !== ids.length || q.complete;
        el("previousQuery").disabled = queryIndex === 0;
        el("nextQuery").disabled = queryIndex === judgments.queries.length - 1;
        el("query").value = String(queryIndex);
    }
    function select(next, focus = true) {
        const oldPage = Math.floor(position / pageSize);
        position = Math.max(0, Math.min(ids.length - 1, next));
        if (oldPage !== Math.floor(position / pageSize)) render(false);
        document.querySelectorAll(".photo").forEach(card => card.classList.toggle("current", Number(card.dataset.position) === position));
        if (focus) document.querySelector(`.photo[data-position="${position}"]`).focus();
    }
    function grade(value) {
        if (value === null) delete query().relevance[order()[position]];
        else query().relevance[order()[position]] = value;
        query().complete = false; dirty = true;
        render(false);
        message("Masz niezapisane zmiany. Pobierz oceny JSON.");
    }
    function render(focus = false) {
        const start = Math.floor(position / pageSize) * pageSize;
        const fragment = document.createDocumentFragment();
        order().slice(start, start + pageSize).forEach((id, offset) => {
            const index = start + offset, card = document.createElement("article");
            card.className = "photo" + (index === position ? " current" : "");
            card.tabIndex = 0; card.dataset.position = String(index);
            card.setAttribute("aria-label", `Zdjęcie ${index + 1}`);
            card.addEventListener("focusin", () => select(index, false));
            card.addEventListener("pointerdown", () => select(index, false));
            const image = document.createElement("img"); image.src = images[id]; image.alt = `Zdjęcie ${index + 1} do oceny`; image.loading = "lazy";
            const caption = document.createElement("small");
            const current = query().relevance[id];
            caption.textContent = `Zdjęcie ${index + 1} · ` + (current === undefined ? "bez oceny" : `ocena ${current}`);
            const buttons = document.createElement("div"); buttons.className = "grades";
            ["0 · Nie", "1 · Częściowo", "2 · Tak"].forEach((label, value) => {
                const button = document.createElement("button"); button.textContent = label;
                button.setAttribute("aria-pressed", String(current === value));
                button.setAttribute("aria-label", `Zdjęcie ${index + 1}: ${label}`);
                button.onclick = () => { position = index; grade(value); select(index); };
                buttons.append(button);
            });
            card.append(image, caption, buttons); fragment.append(card);
        });
        el("grid").replaceChildren(fragment);
        el("page").textContent = `Strona ${Math.floor(position / pageSize) + 1} z ${Math.ceil(ids.length / pageSize)}`;
        el("previousPage").disabled = start === 0;
        el("nextPage").disabled = start + pageSize >= ids.length;
        progress();
        if (focus) select(position);
    }
    function changeQuery(index) { queryIndex = index; position = 0; render(); }
    initial.queries.forEach((q, index) => { const option = document.createElement("option"); option.value = String(index); option.textContent = `${index + 1}. ${q.text}`; el("query").append(option); });
    el("query").onchange = event => changeQuery(Number(event.target.value));
    el("previousQuery").onclick = () => changeQuery(queryIndex - 1);
    el("nextQuery").onclick = () => changeQuery(queryIndex + 1);
    el("previousPage").onclick = () => select(position - pageSize);
    el("nextPage").onclick = () => select(position + pageSize);
    el("complete").onclick = () => { if (Object.keys(query().relevance).length === ids.length) { query().complete = true; dirty = true; progress(); message("Zapytanie zatwierdzone. Pobierz oceny JSON."); } };
    el("download").onclick = () => {
        const url = URL.createObjectURL(new Blob([JSON.stringify(judgments, null, 2) + "\n"], { type: "application/json" }));
        const link = document.createElement("a"); link.href = url; link.download = "judgments.json"; link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        dirty = false; message("Pobrano plik. Użyj go do wznowienia oceny lub obliczenia metryk.");
    };
    el("import").onchange = async event => {
        const file = event.target.files[0]; if (!file) return;
        if (dirty && !confirm("Masz niezapisane zmiany. Zastąpić je wczytanym plikiem?")) { event.target.value = ""; return; }
        try { const next = checked(JSON.parse(await file.text())); judgments = next; dirty = false; render(); message("Wczytano oceny."); }
        catch (error) { message(error.message, true); }
        event.target.value = "";
    };
    document.addEventListener("keydown", event => {
        if (!event.target.closest(".photo") || event.ctrlKey || event.metaKey || event.altKey) return;
        if (["0", "1", "2"].includes(event.key)) { event.preventDefault(); grade(Number(event.key)); select(position); }
        else if (event.key === "Delete") { event.preventDefault(); grade(null); select(position); }
        else if (event.key === "ArrowLeft" || event.key === "ArrowRight") { event.preventDefault(); select(position + (event.key === "ArrowRight" ? 1 : -1)); }
    });
    window.addEventListener("beforeunload", event => { if (dirty) { event.preventDefault(); event.returnValue = ""; } });
    render();
})();
