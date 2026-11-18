// Interactive figure for change_points.md: how a block is cut at modulation change points.
// Mounts into every <div class="tanh-block-split" data-block-size="512">.
//
// The cutting mirrors thl::dsp::BaseProcessor::split_and_process() (src/dsp/BaseProcessor.cpp)
// and the merge thl::modulation::collect_change_points() (include/tanh/modulation/SmartHandle.h);
// the UI points mirror InputEventQueue::drain_spread(). Keep them in step when those change.
(() => {
    "use strict";

    const NS = "http://www.w3.org/2000/svg";
    const DECIMATIONS = [0, 32, 64, 128, 256];
    let instances = 0;

    function option(value, text, selected) {
        return `<option value="${value}"${selected ? " selected" : ""}>${text}</option>`;
    }

    function mount(root) {
        const n = Number(root.dataset.blockSize) > 0 ? Number(root.dataset.blockSize) : 512;
        const id = `tanh-block-split-${++instances}`;
        root.innerHTML = `
            <div class="tbs-controls">
                <label for="${id}-decimation">LFO decimation
                    <select id="${id}-decimation">${DECIMATIONS.map((d) =>
                        option(d, d === 0 ? "off" : `${d} samples`, d === 128)).join("")}</select>
                </label>
                <label for="${id}-events">UI events this block
                    <select id="${id}-events">${[0, 1, 2, 3, 4].map((e) =>
                        option(e, e === 0 ? "none" : String(e), e === 3)).join("")}</select>
                </label>
                <div class="tbs-readout" aria-live="polite"></div>
            </div>
            <div class="tbs-svg-wrap">
                <svg class="tbs-svg" viewBox="0 0 860 300" role="img"
                     aria-label="A ${n}-sample block cut at the merged change points of two parameters"></svg>
            </div>
            <div class="tbs-calls"></div>`;

        const svg = root.querySelector(".tbs-svg");
        const decimationSelect = root.querySelector(`#${id}-decimation`);
        const eventsSelect = root.querySelector(`#${id}-events`);
        const readout = root.querySelector(".tbs-readout");
        const calls = root.querySelector(".tbs-calls");

        // Geometry: the block runs from x0 to x1; three lanes, then the value plot.
        const x0 = 120;
        const x1 = 840;
        const xs = (s) => x0 + (s / n) * (x1 - x0);
        const lanes = { lfo: 40, ui: 78, cut: 116 };
        const plotTop = 158;
        const plotBottom = 268;

        // Example values: threshold = -10 dB plus 3 dB of a sine LFO, held at each step.
        const lfoValue = (s) => -10 + 3 * Math.sin(((2 * Math.PI * s) / n) * 0.75 + 0.6);
        const vMin = -13.5;
        const vMax = -6.5;
        const ys = (v) => plotBottom - ((v - vMin) / (vMax - vMin)) * (plotBottom - plotTop);

        function el(name, attrs, text) {
            const node = document.createElementNS(NS, name);
            for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, String(value));
            if (text !== undefined) node.textContent = text;
            svg.appendChild(node);
            return node;
        }

        function render() {
            const decimation = Number(decimationSelect.value);
            const events = Number(eventsSelect.value);
            svg.replaceChildren();

            const lfoPoints = [];
            if (decimation > 0) for (let s = 0; s < n; s += decimation) lfoPoints.push(s);
            // drain_spread(): the i-th of k events of one stream lands at i * block / k.
            const uiPoints = [];
            for (let i = 0; i < events; i++) uiPoints.push(Math.floor((i * n) / events));

            // collect_change_points(): the union, sorted, without duplicates.
            const merged = [...new Set([...lfoPoints, ...uiPoints])].sort((a, b) => a - b);

            // split_and_process(): the pieces process() is called with.
            const pieces = [];
            let pos = 0;
            for (const cp of merged) {
                if (cp <= pos || cp >= n) continue;
                pieces.push([pos, cp - pos]);
                pos = cp;
            }
            if (pos < n) pieces.push([pos, n - pos]);

            for (const [text, y] of [["LFO (threshold)", lanes.lfo], ["UI (attack)", lanes.ui], ["process() calls", lanes.cut]]) {
                el("text", { x: 8, y: y + 4, class: "tbs-muted" }, text);
                el("line", { x1: x0, y1: y, x2: x1, y2: y, class: "tbs-lane" });
            }

            pieces.forEach(([start, length], i) => {
                const width = xs(start + length) - xs(start);
                const shade = i % 2 ? "tbs-piece-b" : "tbs-piece-a";
                el("rect", { x: xs(start), y: lanes.cut - 12, width, height: 24, class: shade });
                el("rect", { x: xs(start), y: plotTop - 6, width, height: plotBottom - plotTop + 12, class: shade });
                if (width > 26) el("text", { x: xs(start) + 4, y: lanes.cut + 4 }, String(start));
            });

            // A change point at sample 0 cuts nothing: the first piece starts there anyway.
            const tick = (s, y) => el("line", { x1: xs(s), y1: y - 9, x2: xs(s), y2: y + 9, class: s === 0 ? "tbs-tick-idle" : "tbs-tick" });
            lfoPoints.forEach((s) => tick(s, lanes.lfo));
            uiPoints.forEach((s) => tick(s, lanes.ui));

            el("line", { x1: x0, y1: plotBottom + 8, x2: x1, y2: plotBottom + 8, class: "tbs-axis" });
            for (const s of [0, n / 4, n / 2, (3 * n) / 4, n]) {
                el("line", { x1: xs(s), y1: plotBottom + 8, x2: xs(s), y2: plotBottom + 13, class: "tbs-axis" });
                el("text", { x: xs(s), y: plotBottom + 26, class: "tbs-muted", "text-anchor": s === n ? "end" : "middle" }, String(s));
            }
            el("text", { x: 8, y: plotTop + 4, class: "tbs-muted" }, "threshold dB");
            for (const v of [-12, -10, -8]) {
                el("text", { x: x0 - 8, y: ys(v) + 4, class: "tbs-muted", "text-anchor": "end" }, String(v));
            }

            // The value each piece reads at its modulation_offset: the LFO value held since its last step.
            const held = (s) => (decimation === 0 ? -10 : lfoValue(Math.floor(s / decimation) * decimation));
            let stair = "";
            pieces.forEach(([start, length], i) => {
                const y = ys(held(start));
                stair += `${i === 0 ? "M" : "L"}${xs(start)} ${y} L${xs(start + length)} ${y} `;
            });
            el("path", { d: stair, class: "tbs-stair" });

            // A 48-sample linear smoother, retargeted at the start of every piece.
            const ramp = 48;
            let current = held(0);
            let target = current;
            let step = 0;
            let left = 0;
            let smooth = `M${xs(0)} ${ys(current)}`;
            const starts = new Set(pieces.map(([start]) => start));
            for (let s = 0; s <= n; s++) {
                if (starts.has(s) && held(s) !== target) {
                    target = held(s);
                    step = (target - current) / ramp;
                    left = ramp;
                }
                if (left > 0) {
                    current += step;
                    left -= 1;
                    if (left === 0) current = target;
                }
                if (s % 2 === 0 || s === n) smooth += ` L${xs(s)} ${ys(current)}`;
            }
            el("path", { d: smooth, class: "tbs-smooth" });

            const shortest = Math.min(...pieces.map(([, length]) => length));
            readout.innerHTML = `<b>${pieces.length}</b> process() call${pieces.length === 1 ? "" : "s"}` +
                ` · shortest piece <b>${shortest}</b> samples`;
            calls.innerHTML = pieces
                .map(([start, length]) => `process(<span>buffer.sub_block(${start}, ${length})</span>, ${start})`)
                .join("<br>");
        }

        decimationSelect.addEventListener("change", render);
        eventsSelect.addEventListener("change", render);
        render();
    }

    function init() {
        document.querySelectorAll(".tanh-block-split").forEach(mount);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
