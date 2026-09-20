// porydaw scripting API — type declarations (docs/scripting/API.md is the
// reference; this file mirrors it for editors and is hand-maintained; API 1.x.
//
// To get autocomplete when writing code in a plugin folder, create a jsconfig.json with this content:
//   { "compilerOptions": { "checkJs": true }, "include": ["*.js", "path/to/porydaw.d.ts"] }
// or `/// <reference path="…/porydaw.d.ts" />` at the top of main.js.

declare namespace porydaw {
    // ---- basics ----
    const version: string;
    const api: { version: string; major: number };
    const plugin: { id: string; name: string; version: string; dir: string };
    function log(...args: any[]): void;
    function warn(...args: any[]): void;
    function error(...args: any[]): void;

    type Unsubscribe = () => void;

    // ---- project ----
    /** A song's midi.cfg settings (song.settings(), SongInfo.settings). */
    interface SongSettings {
        /** The -G arg, e.g. "_abandoned_ship". */
        voicegroup: string;
        /** Display form of the voicegroup, e.g. "abandoned_ship". */
        voicegroupName: string;
        masterVolume: number;
        /** null while the -R flag is absent (the build then uses the default, 50). */
        reverb: number | null;
        priority: number;
        exactGate: boolean;
        extendedClocks: boolean;
        noCompression: boolean;
        /** The flags as written in midi.cfg. */
        flags: string[];
    }
    interface SongInfo {
        id: number;
        label: string;
        constant: string;
        player: string;
        midPath: string;
        hasMid: boolean;
        registered: boolean;
        /** Registration files still missing the song ("songs.h", ...). */
        registrationGaps: string[];
        settings: SongSettings;
    }
    interface RegistrationStatus {
        complete: boolean;
        inSongTable: boolean;
        inSongsH: boolean;
        inLdScript: boolean;
        inCharmap: boolean;
        inDebugMenu: boolean;
        gaps: string[];
    }
    namespace project {
        const isOpen: boolean;
        const root: string;
        function songs(): SongInfo[];
        function song(label: string): SongInfo | null;
        /** Opens a song by label; false when the project has no playable song of that name. */
        function open(label: string, opts?: { newTab?: boolean }): boolean;
        /** Read fresh from the registration files; throws for an unknown label. */
        function registration(label: string): RegistrationStatus;
        /** File → Register Song without its dialogs; returns the song's table id. Reloads the project. */
        function registerSong(label: string, opts?: { constant?: string; player?: string }): number;
        /** Drops the song's registration lines; the .mid stays. */
        function unregisterSong(label: string): void;
        /** Re-reads the project's music data (song ids may shift). */
        function reload(): void;
        function musicPlayers(): { name: string; number: number; trackCount: number }[];
        /** Every voicegroup: its -G arg and display name. */
        function voicegroups(): { arg: string; name: string }[];
        /** Creates sound/voicegroups/<name>.inc (a copy of `copyFrom`'s voicegroup by arg, or the dummy template); returns the new -G arg. */
        function createVoicegroup(name: string, opts?: { copyFrom?: string }): string;
        /** File → Export Song Bundle without its dialog: a song open in a tab exports with its unsaved edits, any other from disk. `path` follows the io sandbox; a name without a suffix gets `.porysong`, any other suffix throws. */
        function exportBundle(label: string, path: string): { path: string; samples: number };
        /** Imports a .porysong (or bundle folder) into the project and opens the song in a new tab. Omitted options are chosen by the import (`<label>_2` when the label is taken); a given label or constant that is taken throws, as does anything else the import dialog would refuse. Reloads the project; not undoable. */
        function importBundle(
            path: string,
            opts?: { label?: string; constant?: string; player?: string }
        ): { label: string; constant: string; player: string; voicegroup: string; warnings: string[] };
    }

    // ---- song (read-only) ----
    interface Note {
        id: number;
        track: number;
        tick: number;
        key: number;
        len: number;
        vel: number;
    }
    interface LanePoint {
        tick: number;
        value: number;
    }
    interface TimeSig {
        tick: number;
        numerator: number;
        denominator: number;
    }
    interface Track {
        index: number;
        name: string;
        chunk: number;
        channel: number;
        muted: boolean;
        soloed: boolean;
        voice: number;
    }
    type RawEventType =
        | "noteOn"
        | "noteOff"
        | "cc"
        | "program"
        | "bend"
        | "aftertouch"
        | "pressure"
        | "meta"
        | "sysex"
        | "unknown";
    interface RawEvent {
        index: number;
        tick: number;
        status: number;
        type: RawEventType;
        channel?: number;
        data0?: number;
        data1?: number;
        metaType?: number;
        blob?: number[];
        text?: string;
    }
    /** An event to insert or write: `status` or `type` (+ `channel`). */
    interface RawEventSpec {
        tick: number;
        status?: number;
        type?: RawEventType;
        channel?: number;
        data0?: number;
        data1?: number;
        metaType?: number;
        blob?: number[];
        text?: string;
    }
    interface TickRange {
        from?: number;
        to?: number;
    }
    namespace song {
        const CC: {
            MOD: number; VOLUME: number; PAN: number; BEND_RANGE: number; LFO_SPEED: number;
            BEND: number; TEMPO: number; VOICE: number;
        };
        const loaded: boolean;
        /** True in a read-only song bundle tab **/
        const readOnly: boolean;
        const revision: number;
        const label: string;
        const midPath: string;
        const ticksPerBeat: number;
        const ticksPerClock: number;
        const startTempo: number;
        const trackCount: number;
        const trackBudget: number;
        const endTick: number;
        const chunkCount: number;
        function chunkTrack(chunk: number): number;
        function chunkEndTick(chunk: number): number;
        function rawEvents(chunk: number, opts?: TickRange): RawEvent[];
        function settings(): SongSettings;
        /** File → Save (the .mid, its midi.cfg line and the edited voicegroup); refused inside a transaction. */
        function save(): boolean;
        function loop(): { start: number; end: number } | null;
        function timeSigs(): TimeSig[];
        function tracks(): Track[];
        function notes(opts?: TickRange & { track?: number; selectedOnly?: boolean }): Note[];
        function note(id: number): Note | null;
        function lanePoints(track: number, cc: number, opts?: TickRange): LanePoint[];
        /** Once per event-loop turn after the song changed; reactors that edit should return unless origin is "user". */
        function on(event: "changed", fn: (e: { revision: number; origin: "user" | "script" | "history" }) => void): Unsubscribe;
        function on(event: "activated", fn: (e: { label: string } | null) => void): Unsubscribe;
        function off(event: string, fn: Function): void;
    }

    // ---- selection / cursor / view ----
    type NoteArg = number | Note | (number | Note)[];
    interface Lane {
        track: number;
        cc: number;
    }
    interface TimeSelection {
        start: number;
        end: number;
        scope: "tracks" | "lanes";
        lanes: Lane[];
    }
    namespace selection {
        const track: number;
        const trackMask: number;
        function notes(): Note[];
        function time(): TimeSelection | null;
        function setNotes(notes: NoteArg): void;
        function clear(): void;
        function selectTrack(track: number): void;
        function setTime(spec: { start: number; end: number; scope?: "tracks" | "lanes"; lanes?: Lane[] }): void;
        function clearTime(): void;
        function on(event: string, fn: (payload: any) => void): Unsubscribe;
        function off(event: string, fn: Function): void;
    }
    namespace cursor {
        const tick: number;
        function snap(tick: number, mode?: "nearest" | "down" | "up"): number;
        function grid(tick?: number): { start: number; next: number; beatTicks: number; feel: string; minDenom: number };
        function set(tick: number): void;
    }
    namespace view {
        function visibleTicks(): { from: number; to: number } | null;
        function revealTick(tick: number): void;
        function revealRange(from: number, to: number): void;
        function revealNote(note: number | Note): boolean;
        function revealKey(key: number): void;
        let velocityLane: boolean;
        let automationLanes: boolean;
        let tempoLane: boolean;
        let eventList: boolean;
        const pxPerBeat: number;
        const keyHeight: number;
    }

    // ---- edit (inside a transaction) ----
    interface Scope {
        tracks?: number[];
        lanes?: Lane[];
        wholeSong?: boolean;
    }
    namespace edit {
        const active: boolean;
        function transaction<T>(name: string, fn: () => T): T;
        function addNotes(track: number, notes: { tick: number; key: number; len: number; vel: number }[]): number[];
        function deleteNotes(notes: NoteArg): number;
        function moveNotes(notes: NoteArg, dTick: number, dKey: number): number;
        function resizeNotes(notes: NoteArg, dLen: number, opts?: { fromLeft?: boolean }): number;
        function setVelocity(notes: NoteArg, vel: number | ((note: Note) => number)): number;
        function nudgeVelocity(notes: NoteArg, delta: number): number;
        function addLanePoint(track: number, cc: number, tick: number, value: number): void;
        function writeLanePoints(track: number, cc: number, from: number, to: number, points: LanePoint[]): void;
        function moveLanePoints(track: number, cc: number, moves: { tick: number; newTick?: number; newValue?: number }[]): number;
        function deleteLanePoints(track: number, cc: number, ticks: number[]): number;
        function setStartTempo(bpm: number): void;
        function setLoop(start: number | null, end: number | null): void;
        /** Partial update of song.settings(); `voicegroup` accepts the -G arg or the display name. */
        function setSettings(spec: Partial<Omit<SongSettings, "voicegroupName" | "flags">>): void;
        /** Partial update of one editable voice of porydaw.voicegroup (`type` is the macro word). */
        function setVoice(slot: number, spec: Partial<Omit<Voice, "slot" | "kind">>): void;
        function setTimeSig(tick: number, numerator: number, denominator: number): void;
        function deleteTimeSig(tick: number): void;
        function removeTimeRange(start: number, end: number, scope: Scope): boolean;
        function insertTimeRange(at: number, span: number, scope: Scope): boolean;
        function moveRange(start: number, end: number, scope: Scope, dTick: number): number;
        function duplicateRange(start: number, end: number, scope: Scope, dTick: number): number;
        function addTrack(voice?: number): number;
        function duplicateTrack(track: number): number;
        function deleteTrack(track: number): void;
        function mergeTrack(track: number, target: number, options?: { notesOnly?: boolean }): boolean;
        function moveTrack(track: number, target: number): boolean;
        function renameTrack(track: number, name: string): void;
        function transposeSelection(dKey: number): boolean;
        function nudgeSelection(direction: "left" | "right"): boolean;
        function insertRawEvent(chunk: number, event: RawEventSpec): void;
        function modifyRawEvent(chunk: number, index: number, event: RawEventSpec): void;
        function deleteRawEvents(chunk: number, indices: number | number[]): number;
        function moveRawEvent(chunk: number, index: number, destIndex: number): boolean;
        function setChunkEndTick(chunk: number, tick: number): void;
    }

    // ---- transport / audio ----
    type TransportState = "stopped" | "paused" | "playing";
    interface Beat {
        bar: number;
        beat: number;
        beatsPerBar: number;
        beatTicks: number;
        tick: number;
        bpm: number;
    }
    namespace transport {
        const state: TransportState;
        const playheadTick: number;
        const sampleRate: number;
        const loopEnabled: boolean;
        function play(): void;
        function pause(): void;
        function stop(): void;
        function seek(tick: number): void;
        function on(event: "state", fn: (e: { state: TransportState }) => void): Unsubscribe;
        function on(event: "tick", fn: (e: { state: TransportState; tick: number; playing: boolean }) => void): Unsubscribe;
        function on(event: "beat", fn: (e: Beat) => void): Unsubscribe;
        function off(event: string, fn: Function): void;
    }
    interface AudioFrame {
        peak: [number, number];
        rms: [number, number];
        frames: number;
        sampleRate: number;
        playing: boolean;
    }
    interface ChannelState {
        on: boolean;
        releasing: boolean;
        track: number;
        key: number;
    }
    /** The GBA engine settings (Settings → Audio); user-global, not part of a song. */
    interface EngineSettings {
        /** PCM (DirectSound) polyphony, 1..engineLimits().maxPcmChannels. */
        maxPcmChannels: number;
        /** DirectSound mix rate in Hz: 0 = the host rate, else one of engineLimits().mixRates. */
        pcmMixRate: number;
        /** The GBA analog output low-pass. */
        analogFilter: boolean;
    }
    namespace audio {
        const sampleRate: number;
        const windowFrames: number;
        const peak: [number, number];
        const rms: [number, number];
        function pcm(): Float32Array;
        function spectrum(bins?: number): Float32Array;
        function channels(): { pcm: ChannelState[]; cgb: ChannelState[]; maxPcm: number; activePcm: number; activeCgb: number } | null;
        /** Renders the active song to a WAV file (blocks; refused inside a transaction). */
        function render(path: string, opts?: { sampleRate?: number; loopCount?: number; fadeout?: number; tail?: number }): { path: string; seconds: number };
        const engine: EngineSettings;
        function engineLimits(): { maxPcmChannels: number; mixRates: number[] };
        /** Partial update; persists, updates Settings → Audio and restarts the audio device. Throws on unknown keys or bad values. */
        function setEngine(spec: Partial<EngineSettings>): void;
        function on(event: "frame", fn: (frame: AudioFrame) => void): Unsubscribe;
        function on(event: "engine", fn: (settings: EngineSettings) => void): Unsubscribe;
        function off(event: string, fn: Function): void;
    }

    // ---- actions ----
    namespace actions {
        function register(spec: {
            id: string;
            name: string;
            context?: "global" | "roll" | "velocity" | "range";
            default?: string;
            run: () => void;
        }): string;
        function unregister(fullId: string): void;
    }

    // ---- ui ----
    type Color = string | [number, number, number] | [number, number, number, number];
    interface TextOptions {
        size?: number;
        bold?: boolean;
        align?: "left" | "center" | "right";
        baseline?: "alphabetic" | "top" | "middle" | "bottom";
    }
    type Points = number[] | { x: number; y: number }[];
    /** The canvas painter, valid only inside a paint callback. */
    interface Painter {
        readonly width: number;
        readonly height: number;
        readonly dpr: number;
        clear(color: Color): void;
        fillRect(x: number, y: number, w: number, h: number, color: Color): void;
        strokeRect(x: number, y: number, w: number, h: number, color: Color, lineWidth?: number): void;
        fillRoundRect(x: number, y: number, w: number, h: number, radius: number, color: Color): void;
        line(x1: number, y1: number, x2: number, y2: number, color: Color, lineWidth?: number): void;
        fillCircle(cx: number, cy: number, r: number, color: Color): void;
        strokeCircle(cx: number, cy: number, r: number, color: Color, lineWidth?: number): void;
        fillEllipse(x: number, y: number, w: number, h: number, color: Color): void;
        fillPolygon(points: Points, color: Color): void;
        strokePolyline(points: Points, color: Color, lineWidth?: number, close?: boolean): void;
        text(x: number, y: number, text: string, color: Color, opts?: TextOptions): void;
        measureText(text: string, opts?: TextOptions): { width: number; height: number; ascent: number; descent: number };
        image(id: number, dx: number, dy: number, dw?: number, dh?: number, sx?: number, sy?: number, sw?: number, sh?: number): void;
        save(): void;
        restore(): void;
        translate(dx: number, dy: number): void;
        rotate(degrees: number): void;
        scale(sx: number, sy: number): void;
        opacity(alpha: number): void;
        clip(x: number, y: number, w: number, h: number): void;
        antialias(on: boolean): void;
    }
    interface MouseEvent {
        type: "press" | "move" | "release" | "doubleclick" | "leave" | "wheel";
        x: number;
        y: number;
        button: "left" | "right" | "middle" | "none";
        left: boolean;
        right: boolean;
        middle: boolean;
        deltaX?: number;
        deltaY?: number;
    }
    interface Widget {
        readonly kind: string;
        text: string;
        value: number;
        checked: boolean;
        index: number;
        enabled: boolean;
        visible: boolean;
        readonly width: number;
        readonly height: number;
        setMinimumSize(w: number, h: number): void;
        setToolTip(text: string): void;
        repaint(): void;
        setItems(items: string[]): void;
    }
    interface Container extends Widget {
        addLabel(text: string): Widget;
        addButton(text: string, onClick: () => void): Widget;
        addCheckbox(text: string, checked: boolean, onChange: (on: boolean) => void): Widget;
        addSlider(min: number, max: number, value: number, onChange: (v: number) => void, opts?: { vertical?: boolean }): Widget;
        addCombo(items: string[], index: number, onChange: (i: number) => void): Widget;
        addCanvas(opts: { minWidth?: number; minHeight?: number }, paint: (g: Painter) => void, mouse?: (ev: MouseEvent) => void): Widget;
        addRow(): Container;
        addColumn(): Container;
        addStretch(): void;
        addSpacing(px: number): void;
    }
    interface Dock {
        readonly id: string;
        title: string;
        visible: boolean;
        readonly open: boolean;
        readonly root: Container;
        readonly canvas: Widget | null;
        show(): void;
        hide(): void;
        raise(): void;
        close(): void;
    }
    interface MenuItem {
        label: string;
        enabled: boolean;
        visible: boolean;
        checked: boolean;
        remove(): void;
    }
    interface MenuItemSpec {
        label: string;
        run?: (checked: boolean) => void;
        /** Asked each time the menu opens; return false to leave the entry out of that opening. */
        shouldShow?: () => boolean;
        /** A full command id from actions.register: shows its binding, and runs it without `run`. */
        action?: string;
        checkable?: boolean;
        checked?: boolean;
        enabled?: boolean;
        tooltip?: string;
    }
    interface Menu {
        label: string;
        enabled: boolean;
        visible: boolean;
        addItem(spec: MenuItemSpec): MenuItem;
        /** Menu-bar menus only. */
        addSeparator(): void;
        /** Menu-bar menus only. */
        addMenu(label: string): Menu;
        clear(): void;
    }
    /** The roll geometry an overlay paint receives; (0, 0) is the note area's top-left. */
    interface OverlayView {
        readonly width: number;
        readonly height: number;
        readonly from: number;
        readonly to: number;
        readonly keyHeight: number;
        readonly pxPerBeat: number;
        readonly track: number;
        x(tick: number): number;
        tick(x: number): number;
        keyTop(key: number): number;
        keyBottom(key: number): number;
        key(y: number): number;
    }
    interface Overlay {
        readonly id: string;
        visible: boolean;
        readonly active: boolean;
        repaint(): void;
        remove(): void;
    }
    interface FormField {
        key: string;
        label?: string;
        type?: "text" | "number" | "checkbox" | "combo";
        value?: string | number | boolean;
        min?: number;
        max?: number;
        step?: number;
        decimals?: number;
        items?: string[];
        placeholder?: string;
    }
    namespace ui {
        function statusMessage(text: string): void;
        function theme(role: string): string;
        function theme(): { [role: string]: string };
        function loadImage(path: string): number;
        function imageSize(id: number): { width: number; height: number } | null;
        function freeImage(id: number): void;
        function dock(spec: {
            id: string;
            title?: string;
            area?: "left" | "right" | "top" | "bottom";
            minWidth?: number;
            minHeight?: number;
            paint?: (g: Painter) => void;
            mouse?: (ev: MouseEvent) => void;
            build?: (root: Container) => void;
        }): Dock;
        /** The plugin's submenu of the menu bar's Plugins menu. */
        function menu(): Menu;
        /** Items appended to the roll's note menu or the time-selection menu. */
        function contextMenu(surface: "notes" | "range"): Menu;
        function overlay(spec: { id: string; paint: (g: Painter, v: OverlayView) => void }): Overlay;
        namespace dialog {
            function alert(text: string, opts?: { title?: string; detail?: string; ok?: string }): void;
            function confirm(text: string, opts?: { title?: string; detail?: string; ok?: string; cancel?: string }): boolean;
            function prompt(text: string, opts?: { title?: string; value?: string; ok?: string; cancel?: string }): string | null;
            function form(spec: { title?: string; text?: string; ok?: string; cancel?: string; fields: FormField[] }): { [key: string]: any } | null;
            function openFile(opts?: { title?: string; dir?: string; filter?: string }): string | null;
            function saveFile(opts?: { title?: string; dir?: string; filter?: string; name?: string }): string | null;
            function chooseDir(opts?: { title?: string; dir?: string }): string | null;
        }
    }

    // ---- voicegroup (read-only; edits via edit.setVoice) ----
    type VoiceType =
        | "voice_directsound" | "voice_directsound_no_resample" | "voice_directsound_alt"
        | "voice_square_1" | "voice_square_1_alt" | "voice_square_2" | "voice_square_2_alt"
        | "voice_programmable_wave" | "voice_programmable_wave_alt"
        | "voice_noise" | "voice_noise_alt" | "voice_keysplit" | "voice_keysplit_all";
    /** One of the 128 slots of the song's voicegroup file. */
    interface VoiceSlot {
        slot: number;
        /** "voice" = editable (a Voice); "cry" = read-only; "broken" = unparseable; "empty" = past the last voice; "other". */
        kind: "voice" | "cry" | "broken" | "empty" | "other";
    }
    /** An editable voice: the macro's arguments as written in the .inc file. */
    interface Voice extends VoiceSlot {
        kind: "voice";
        type: VoiceType;
        key: number;
        pan: number;
        /** Sample / wave symbol, or the keysplit / drumkit sub-voicegroup. */
        symbol: string;
        /** voice_keysplit only. */
        keysplitTable: string;
        /** voice_square_1 only. */
        sweep: number;
        /** square voices only (0–3). */
        duty: number;
        /** noise only (0–1). */
        period: number;
        /** CGB: A/D/R 0–7, S 0–15; DirectSound: 0–255. */
        attack: number;
        decay: number;
        sustain: number;
        release: number;
    }
    interface Adsr { attack: number; decay: number; sustain: number; release: number }
    namespace voicegroup {
        /** False when no song is open or the layout can't be edited. */
        const isOpen: boolean;
        /** The -G arg and its display name (from the song's settings). */
        const arg: string;
        const name: string;
        const file: string;
        /** The name the engine loads the voicegroup by. */
        const loadName: string;
        /** Unsaved voice edits exist (song.save() writes them). */
        const dirty: boolean;
        const monolithic: boolean;
        function voices(): (VoiceSlot | Voice)[];
        function voice(slot: number): VoiceSlot | Voice | null;
        function symbols(): {
            directSound: string[];
            progWave: string[];
            drumkits: string[];
            synths: string[];
            keysplits: { voicegroup: string; table: string }[];
        };
        /** The envelope the dock proposes for a voice type (and sample symbol). */
        function typicalAdsr(type: VoiceType | string, symbol?: string): Adsr;
    }

    // ---- io (sandboxed) ----
    namespace io {
        const pluginDir: string;
        const projectRoot: string;
        function resolve(path: string): string;
        function exists(path: string): boolean;
        function isDir(path: string): boolean;
        function readText(path: string): string;
        function writeText(path: string, text: string): void;
        function readBytes(path: string): Uint8Array;
        function writeBytes(path: string, bytes: Uint8Array | ArrayBuffer): void;
        function list(dir: string): { name: string; dir: boolean; size: number }[];
        function mkdir(path: string): void;
        function remove(path: string): void;
    }

    // ---- storage ----
    namespace storage {
        function get<T>(key: string, fallback?: T): T;
        function set(key: string, value: any): void;
        function remove(key: string): void;
        function keys(): string[];
        /** Per-song values, kept in the song's sidecar. */
        namespace song {
            function get<T>(key: string, fallback?: T): T;
            function set(key: string, value: any): void;
            function remove(key: string): void;
            function keys(): string[];
        }
    }
}

declare const console: {
    log(...args: any[]): void;
    info(...args: any[]): void;
    debug(...args: any[]): void;
    warn(...args: any[]): void;
    error(...args: any[]): void;
};
