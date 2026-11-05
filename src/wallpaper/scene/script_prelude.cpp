#include "wallpaper/scene/script_prelude.hpp"

// The JavaScript side of SceneScript (docs/FORMATS.md, 11): the Vec classes, the global
// objects and the layer objects scripts see. Everything that needs the scene goes through
// `__native`, which script.cpp provides; everything else lives here, in plain JavaScript.
namespace glint::scene {

const char* const script_prelude = R"JS(
'use strict';

function __num(v) { return typeof v === 'number' ? v : (v === true ? 1 : v === false ? 0 : Number(v)); }
function __parseFloats(s) {
    const out = [];
    for (const part of String(s).trim().split(/\s+/)) {
        if (part === '') continue;
        const f = parseFloat(part);
        out.push(isNaN(f) ? 0 : f);
    }
    return out;
}

class Vec2 {
    constructor(x, y) {
        if (x === undefined) { this.x = 0; this.y = 0; }
        else if (typeof x === 'string') { const f = __parseFloats(x); this.x = f[0] || 0; this.y = f[1] || 0; }
        else if (typeof x === 'object' && x !== null) { this.x = __num(x.x); this.y = __num(x.y); }
        else if (y === undefined) { this.x = __num(x); this.y = __num(x); }
        else { this.x = __num(x); this.y = __num(y); }
    }
    static __of(v) { return v instanceof Vec2 ? v : typeof v === 'object' && v !== null ? new Vec2(v.x, v.y) : new Vec2(v); }
    length() { return Math.sqrt(this.x * this.x + this.y * this.y); }
    lengthSqr() { return this.x * this.x + this.y * this.y; }
    distance(o) { return this.subtract(o).length(); }
    distanceSqr(o) { return this.subtract(o).lengthSqr(); }
    normalize() { const l = this.length(); return l > 0 ? new Vec2(this.x / l, this.y / l) : new Vec2(0, 0); }
    copy() { return new Vec2(this.x, this.y); }
    equals(o) { return Math.abs(this.x - o.x) < 1e-6 && Math.abs(this.y - o.y) < 1e-6; }
    isFinite() { return Number.isFinite(this.x) && Number.isFinite(this.y); }
    negate() { return new Vec2(-this.x, -this.y); }
    add(v) { const o = Vec2.__of(v); return new Vec2(this.x + o.x, this.y + o.y); }
    subtract(v) { const o = Vec2.__of(v); return new Vec2(this.x - o.x, this.y - o.y); }
    multiply(v) { const o = Vec2.__of(v); return new Vec2(this.x * o.x, this.y * o.y); }
    divide(v) { const o = Vec2.__of(v); return new Vec2(this.x / o.x, this.y / o.y); }
    dot(o) { return this.x * o.x + this.y * o.y; }
    reflect(n) { const d = 2 * this.dot(n); return new Vec2(this.x - d * n.x, this.y - d * n.y); }
    perpendicular() { return new Vec2(-this.y, this.x); }
    project(o) { const l = o.lengthSqr(); return l > 0 ? o.multiply(this.dot(o) / l) : new Vec2(0, 0); }
    angle() { return Math.atan2(this.y, this.x) * 180 / Math.PI; }
    angleBetween(o) { const d = this.length() * o.length(); return d > 0 ? Math.acos(Math.max(-1, Math.min(1, this.dot(o) / d))) * 180 / Math.PI : 0; }
    rotate(a) { const r = a * Math.PI / 180, c = Math.cos(r), s = Math.sin(r); return new Vec2(this.x * c - this.y * s, this.x * s + this.y * c); }
    mix(o, t) { const a = Vec2.__of(t); return new Vec2(this.x + (o.x - this.x) * a.x, this.y + (o.y - this.y) * a.y); }
    min(o) { return new Vec2(Math.min(this.x, o.x), Math.min(this.y, o.y)); }
    max(o) { return new Vec2(Math.max(this.x, o.x), Math.max(this.y, o.y)); }
    clamp(lo, hi) { const a = Vec2.__of(lo), b = Vec2.__of(hi); return new Vec2(Math.min(Math.max(this.x, a.x), b.x), Math.min(Math.max(this.y, a.y), b.y)); }
    abs() { return new Vec2(Math.abs(this.x), Math.abs(this.y)); }
    sign() { return new Vec2(Math.sign(this.x), Math.sign(this.y)); }
    round() { return new Vec2(Math.round(this.x), Math.round(this.y)); }
    floor() { return new Vec2(Math.floor(this.x), Math.floor(this.y)); }
    ceil() { return new Vec2(Math.ceil(this.x), Math.ceil(this.y)); }
    fract() { return new Vec2(this.x - Math.floor(this.x), this.y - Math.floor(this.y)); }
    mod(v) { const o = Vec2.__of(v); return new Vec2(this.x - o.x * Math.floor(this.x / o.x), this.y - o.y * Math.floor(this.y / o.y)); }
    step(e) { const o = Vec2.__of(e); return new Vec2(this.x < o.x ? 0 : 1, this.y < o.y ? 0 : 1); }
    smoothStep(lo, hi) { const a = Vec2.__of(lo), b = Vec2.__of(hi); return new Vec2(WEMath.smoothStep(a.x, b.x, this.x), WEMath.smoothStep(a.y, b.y, this.y)); }
    toString() { return this.x + ' ' + this.y; }
}

class Vec3 {
    constructor(x, y, z) {
        if (x === undefined) { this.x = 0; this.y = 0; this.z = 0; }
        else if (typeof x === 'string') { const f = __parseFloats(x); this.x = f[0] || 0; this.y = f[1] || 0; this.z = f[2] || 0; }
        else if (typeof x === 'object' && x !== null) { this.x = __num(x.x); this.y = __num(x.y); this.z = x.z === undefined ? 0 : __num(x.z); }
        else if (y === undefined) { this.x = __num(x); this.y = __num(x); this.z = __num(x); }
        else { this.x = __num(x); this.y = __num(y); this.z = z === undefined ? 0 : __num(z); }
    }
    static __of(v) { return v instanceof Vec3 ? v : typeof v === 'object' && v !== null ? new Vec3(v.x, v.y, v.z === undefined ? 0 : v.z) : new Vec3(v); }
    static fromSpherical(r, theta, phi) {
        const t = theta * Math.PI / 180, p = phi * Math.PI / 180;
        return new Vec3(r * Math.sin(t) * Math.cos(p), r * Math.sin(t) * Math.sin(p), r * Math.cos(t));
    }
    length() { return Math.sqrt(this.x * this.x + this.y * this.y + this.z * this.z); }
    lengthSqr() { return this.x * this.x + this.y * this.y + this.z * this.z; }
    distance(o) { return this.subtract(o).length(); }
    distanceSqr(o) { return this.subtract(o).lengthSqr(); }
    normalize() { const l = this.length(); return l > 0 ? new Vec3(this.x / l, this.y / l, this.z / l) : new Vec3(0, 0, 0); }
    copy() { return new Vec3(this.x, this.y, this.z); }
    equals(o) { return Math.abs(this.x - o.x) < 1e-6 && Math.abs(this.y - o.y) < 1e-6 && Math.abs(this.z - o.z) < 1e-6; }
    isFinite() { return Number.isFinite(this.x) && Number.isFinite(this.y) && Number.isFinite(this.z); }
    negate() { return new Vec3(-this.x, -this.y, -this.z); }
    add(v) { const o = Vec3.__of(v); return new Vec3(this.x + o.x, this.y + o.y, this.z + o.z); }
    subtract(v) { const o = Vec3.__of(v); return new Vec3(this.x - o.x, this.y - o.y, this.z - o.z); }
    multiply(v) { const o = Vec3.__of(v); return new Vec3(this.x * o.x, this.y * o.y, this.z * o.z); }
    divide(v) { const o = Vec3.__of(v); return new Vec3(this.x / o.x, this.y / o.y, this.z / o.z); }
    dot(o) { return this.x * o.x + this.y * o.y + this.z * o.z; }
    cross(o) { return new Vec3(this.y * o.z - this.z * o.y, this.z * o.x - this.x * o.z, this.x * o.y - this.y * o.x); }
    reflect(n) { const d = 2 * this.dot(n); return new Vec3(this.x - d * n.x, this.y - d * n.y, this.z - d * n.z); }
    refract(n, eta) {
        const d = n.dot(this), k = 1 - eta * eta * (1 - d * d);
        if (k < 0) return new Vec3(0, 0, 0);
        const f = eta * d + Math.sqrt(k);
        return new Vec3(eta * this.x - f * n.x, eta * this.y - f * n.y, eta * this.z - f * n.z);
    }
    project(o) { const l = o.lengthSqr(); return l > 0 ? o.multiply(this.dot(o) / l) : new Vec3(0, 0, 0); }
    angleBetween(o) { const d = this.length() * o.length(); return d > 0 ? Math.acos(Math.max(-1, Math.min(1, this.dot(o) / d))) * 180 / Math.PI : 0; }
    toSpherical() { const r = this.length(); return r > 0 ? new Vec3(r, Math.acos(this.z / r) * 180 / Math.PI, Math.atan2(this.y, this.x) * 180 / Math.PI) : new Vec3(0, 0, 0); }
    mix(o, t) { const a = Vec3.__of(t); return new Vec3(this.x + (o.x - this.x) * a.x, this.y + (o.y - this.y) * a.y, this.z + (o.z - this.z) * a.z); }
    min(o) { return new Vec3(Math.min(this.x, o.x), Math.min(this.y, o.y), Math.min(this.z, o.z)); }
    max(o) { return new Vec3(Math.max(this.x, o.x), Math.max(this.y, o.y), Math.max(this.z, o.z)); }
    clamp(lo, hi) { const a = Vec3.__of(lo), b = Vec3.__of(hi); return new Vec3(Math.min(Math.max(this.x, a.x), b.x), Math.min(Math.max(this.y, a.y), b.y), Math.min(Math.max(this.z, a.z), b.z)); }
    abs() { return new Vec3(Math.abs(this.x), Math.abs(this.y), Math.abs(this.z)); }
    sign() { return new Vec3(Math.sign(this.x), Math.sign(this.y), Math.sign(this.z)); }
    round() { return new Vec3(Math.round(this.x), Math.round(this.y), Math.round(this.z)); }
    floor() { return new Vec3(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z)); }
    ceil() { return new Vec3(Math.ceil(this.x), Math.ceil(this.y), Math.ceil(this.z)); }
    fract() { return new Vec3(this.x - Math.floor(this.x), this.y - Math.floor(this.y), this.z - Math.floor(this.z)); }
    mod(v) { const o = Vec3.__of(v); return new Vec3(this.x - o.x * Math.floor(this.x / o.x), this.y - o.y * Math.floor(this.y / o.y), this.z - o.z * Math.floor(this.z / o.z)); }
    step(e) { const o = Vec3.__of(e); return new Vec3(this.x < o.x ? 0 : 1, this.y < o.y ? 0 : 1, this.z < o.z ? 0 : 1); }
    smoothStep(lo, hi) { const a = Vec3.__of(lo), b = Vec3.__of(hi); return new Vec3(WEMath.smoothStep(a.x, b.x, this.x), WEMath.smoothStep(a.y, b.y, this.y), WEMath.smoothStep(a.z, b.z, this.z)); }
    toString() { return this.x + ' ' + this.y + ' ' + this.z; }
}

class Vec4 {
    constructor(x, y, z, w) {
        if (x === undefined) { this.x = this.y = this.z = this.w = 0; }
        else if (typeof x === 'string') { const f = __parseFloats(x); this.x = f[0] || 0; this.y = f[1] || 0; this.z = f[2] || 0; this.w = f[3] || 0; }
        else if (typeof x === 'object' && x !== null) { this.x = __num(x.x); this.y = __num(x.y); this.z = __num(x.z || 0); this.w = __num(x.w || 0); }
        else if (y === undefined) { this.x = this.y = this.z = this.w = __num(x); }
        else { this.x = __num(x); this.y = __num(y); this.z = __num(z || 0); this.w = __num(w || 0); }
    }
    copy() { return new Vec4(this.x, this.y, this.z, this.w); }
    toString() { return this.x + ' ' + this.y + ' ' + this.z + ' ' + this.w; }
}

// Column-major 4x4, as Wallpaper Engine keeps it.
class Mat4 {
    constructor(m) { this.m = m ? Array.from(m) : [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]; }
    copy() { return new Mat4(this.m); }
    multiply(o) {
        const a = this.m, b = o.m, r = new Array(16);
        for (let c = 0; c < 4; ++c) for (let row = 0; row < 4; ++row) {
            let s = 0;
            for (let k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
        return new Mat4(r);
    }
    transformPoint(v) {
        const m = this.m;
        return new Vec3(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13], m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]);
    }
    getOrigin() { return new Vec3(this.m[12], this.m[13], this.m[14]); }
}

class Mat3 {
    constructor(m) { this.m = m ? Array.from(m) : [1,0,0, 0,1,0, 0,0,1]; }
    copy() { return new Mat3(this.m); }
}

const WEMath = {
    smoothStep(lo, hi, v) { const t = Math.max(0, Math.min(1, (v - lo) / (hi - lo))); return t * t * (3 - 2 * t); },
    mix(a, b, t) { return a + (b - a) * t; },
    deg2rad: Math.PI / 180,
    rad2deg: 180 / Math.PI,
};
const WEVector = {
    angleVector2(angle) { const r = angle * Math.PI / 180; return new Vec2(Math.cos(r), Math.sin(r)); },
    vectorAngle2(d) { return Math.atan2(d.y, d.x) * 180 / Math.PI; },
};
const WEColor = {
    rgb2hsv(c) {
        const r = c.x, g = c.y, b = c.z, mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
        let h = 0;
        if (d > 0) {
            if (mx === r) h = ((g - b) / d) % 6;
            else if (mx === g) h = (b - r) / d + 2;
            else h = (r - g) / d + 4;
            h /= 6;
            if (h < 0) h += 1;
        }
        return new Vec3(h, mx > 0 ? d / mx : 0, mx);
    },
    hsv2rgb(c) {
        const h = ((c.x % 1) + 1) % 1, s = c.y, v = c.z, i = Math.floor(h * 6), f = h * 6 - i;
        const p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        switch (i % 6) {
            case 0: return new Vec3(v, t, p);
            case 1: return new Vec3(q, v, p);
            case 2: return new Vec3(p, v, t);
            case 3: return new Vec3(p, q, v);
            case 4: return new Vec3(t, p, v);
            default: return new Vec3(v, p, q);
        }
    },
    normalizeColor(c) { return new Vec3(c.x / 255, c.y / 255, c.z / 255); },
    expandColor(c) { return new Vec3(c.x * 255, c.y * 255, c.z * 255); },
};
globalThis.WEMath = WEMath;
globalThis.WEVector = WEVector;
globalThis.WEColor = WEColor;
globalThis.Vec2 = Vec2;
globalThis.Vec3 = Vec3;
globalThis.Vec4 = Vec4;
globalThis.Mat4 = Mat4;
globalThis.Mat3 = Mat3;

class MediaPlaybackEvent { }
MediaPlaybackEvent.PLAYBACK_STOPPED = 0;
MediaPlaybackEvent.PLAYBACK_PLAYING = 1;
MediaPlaybackEvent.PLAYBACK_PAUSED = 2;
globalThis.MediaPlaybackEvent = MediaPlaybackEvent;
globalThis.CursorEvent = class CursorEvent { };
globalThis.AnimationEvent = class AnimationEvent { };
globalThis.MediaPropertiesEvent = class MediaPropertiesEvent { };
globalThis.MediaThumbnailEvent = class MediaThumbnailEvent { };
globalThis.MediaTimelineEvent = class MediaTimelineEvent { };
globalThis.MediaStatusEvent = class MediaStatusEvent { };

// Values cross the bridge as JSON scalars: vectors as "x y z" strings. Which fields are
// text and which are vectors is known by name; anything else is told by its shape.
const __stringFields = new Set(['text', 'font', 'name', 'horizontalalign', 'verticalalign', 'anchor', 'alignment', 'file', 'mode']);
const __vec3Fields = new Set(['origin', 'scale', 'angles', 'color', 'backgroundcolor', 'clearcolor', 'ambientcolor', 'skylightcolor', 'colorn', 'colorstart', 'colorend']);
const __vec2Fields = new Set(['parallaxDepth', 'size', 'offset', 'spacing', 'padding', 'pointerposition']);
function __wrap(name, raw) {
    if (raw === undefined || raw === null) return undefined;
    if (__stringFields.has(name)) return String(raw);
    if (typeof raw === 'string') {
        // The shape decides: an effect's "scale" may be two numbers, a layer's three.
        const parts = raw.trim().split(/\s+/).filter(p => p !== '');
        const f = __parseFloats(raw);
        const numeric = parts.length > 0 && parts.every(p => /^-?(\d+\.?\d*|\.\d+)(e-?\d+)?$/i.test(p));
        if (numeric && parts.length === 4) return new Vec4(f[0], f[1], f[2], f[3]);
        if (numeric && parts.length === 3) return new Vec3(f[0], f[1], f[2]);
        if (numeric && parts.length === 2) return new Vec2(f[0], f[1]);
        if (numeric && parts.length === 1) {
            if (__vec3Fields.has(name)) return new Vec3(f[0], f[0], f[0]);
            if (__vec2Fields.has(name)) return new Vec2(f[0], f[0]);
            return f[0];
        }
        if (__vec3Fields.has(name)) return new Vec3(f[0] || 0, f[1] || 0, f[2] || 0);
        if (__vec2Fields.has(name)) return new Vec2(f[0] || 0, f[1] || 0);
        return raw;
    }
    if (typeof raw === 'number') {
        if (__vec3Fields.has(name)) return new Vec3(raw, raw, raw);
        if (__vec2Fields.has(name)) return new Vec2(raw, raw);
    }
    return raw;
}
function __unwrap(name, v, like) {
    if (v === undefined || v === null) return undefined;
    if (v instanceof Vec2 || v instanceof Vec3 || v instanceof Vec4) return v.toString();
    if (typeof v === 'object') {
        if ('z' in v) return __num(v.x) + ' ' + __num(v.y) + ' ' + __num(v.z);
        if ('y' in v) return __num(v.x) + ' ' + __num(v.y);
    }
    if (typeof v === 'number') {
        // A number for a vector: every component (the docs: 2 for scale is 2 2 2).
        if (__vec3Fields.has(name) || like instanceof Vec3) return v + ' ' + v + ' ' + v;
        if (__vec2Fields.has(name) || like instanceof Vec2) return v + ' ' + v;
        return v;
    }
    if (typeof v === 'boolean') return v;
    return String(v);
}
// Angles are degrees for scripts and radians in the scene.
function __toScript(name, raw) {
    const v = __wrap(name, raw);
    if (name === 'angles' && v instanceof Vec3) return v.multiply(180 / Math.PI);
    return v;
}
function __toScene(name, v, like) {
    if (name === 'angles') {
        if (typeof v === 'number') v = new Vec3(v, v, v);
        if (v instanceof Vec3) v = v.multiply(Math.PI / 180);
    }
    return __unwrap(name, v, like);
}

// IAnimation on a field's timeline.
class Animation {
    constructor(key) { this.__key = key; }
    get fps() { return __native.animation(this.__key, 'fps'); }
    get frameCount() { return __native.animation(this.__key, 'frameCount'); }
    get duration() { return __native.animation(this.__key, 'duration'); }
    get name() { return __native.animation(this.__key, 'name'); }
    get rate() { return __native.animation(this.__key, 'rate'); }
    set rate(v) { __native.animation(this.__key, 'setRate', __num(v)); }
    play() { __native.animation(this.__key, 'play'); }
    pause() { __native.animation(this.__key, 'pause'); }
    stop() { __native.animation(this.__key, 'stop'); }
    isPlaying() { return __native.animation(this.__key, 'isPlaying'); }
    getFrame() { return __native.animation(this.__key, 'getFrame'); }
    setFrame(f) { __native.animation(this.__key, 'setFrame', __num(f)); }
}

// What a layer has no timeline or texture animation for: answers that keep a script going.
class NoAnimation {
    get fps() { return 30; }
    get frameCount() { return 0; }
    get duration() { return 0; }
    get name() { return ''; }
    get rate() { return 1; }
    set rate(v) { }
    get blend() { return 1; }
    set blend(v) { }
    get visible() { return true; }
    set visible(v) { }
    play() { }
    pause() { }
    stop() { }
    join() { }
    isPlaying() { return false; }
    getFrame() { return 0; }
    setFrame(f) { }
    addEndedCallback(f) { }
}

const __layerFields = ['origin', 'angles', 'scale', 'parallaxDepth', 'visible', 'alpha', 'color', 'brightness',
    'size', 'text', 'pointsize', 'font', 'padding', 'horizontalalign', 'verticalalign', 'anchor',
    'limitrows', 'maxrows', 'limitwidth', 'maxwidth', 'opaquebackground', 'backgroundcolor',
    'perspective', 'solid', 'volume', 'count', 'rate', 'speed', 'lifetime', 'colorn'];

class Layer {
    constructor(kind, index) {
        Object.defineProperty(this, '__kind', { value: kind });
        Object.defineProperty(this, '__index', { value: index });
        for (const name of __layerFields) {
            Object.defineProperty(this, name, {
                enumerable: true,
                get() {
                    const raw = __native.get(this.__kind, this.__index, name);
                    if (raw === undefined) return this['__own_' + name];
                    return __toScript(name, raw);
                },
                set(v) {
                    const like = __toScript(name, __native.get(this.__kind, this.__index, name));
                    if (!__native.set(this.__kind, this.__index, name, __toScene(name, v, like))) {
                        this['__own_' + name] = v;
                    }
                },
            });
        }
    }
    get name() { return __native.get(this.__kind, this.__index, 'name'); }
    set name(v) { __native.set(this.__kind, this.__index, 'name', String(v)); }
    get id() { return __native.get(this.__kind, this.__index, 'id'); }
    getParent() { const p = __native.parent(this.__kind, this.__index); return p ? __glint.layer(p[0], p[1]) : undefined; }
    getChildren() { return __native.children(this.__kind, this.__index).map(c => __glint.layer(c[0], c[1])); }
    setParent(parent, a, b) { __native.warn('setParent is not supported'); }
    getAnimation(name) {
        const key = __native.animationKey(this.__kind, this.__index, name === undefined ? '' : String(name));
        return key >= 0 ? new Animation(key) : new NoAnimation();
    }
    getTextureAnimation() { return new NoAnimation(); }
    getVideoTexture() { return new NoAnimation(); }
    getAnimationLayerCount() { return __native.get(this.__kind, this.__index, 'animationLayerCount') || 0; }
    getAnimationLayer(which) { return __glint.animationLayer(this.__kind, this.__index, which); }
    createAnimationLayer(a, c) { __native.warn('createAnimationLayer is not supported'); return new NoAnimation(); }
    playSingleAnimation(a, c) { __native.warn('playSingleAnimation is not supported'); return new NoAnimation(); }
    destroyAnimationLayer(a) { return false; }
    getEffect(which) { return __glint.effect(this.__kind, this.__index, which); }
    getEffectCount() { return __native.get(this.__kind, this.__index, 'effectCount') || 0; }
    getTransformMatrix() { return new Mat4(__native.transform(this.__kind, this.__index)); }
    rotateObjectSpace(a) { this.angles = this.angles.add(a); }
    lookAt(c, u) { }
    lookAtYaw(c, u) { }
    getAttachmentIndex(n) { return -1; }
    getAttachmentMatrix(a) { return new Mat4(); }
    getAttachmentOrigin(a) { return this.origin; }
    getAttachmentAngles(a) { return new Vec3(); }
    getBoneCount() { return __native.get(this.__kind, this.__index, 'boneCount') || 0; }
    getBoneIndex(n) { return -1; }
    play() { }
    pause() { }
    stop() { }
    isPlaying() { return false; }
    emitParticles(n) { }
    get instance() { return this; }
}

class AnimationLayerRef {
    constructor(kind, index, which) {
        Object.defineProperty(this, '__kind', { value: kind });
        Object.defineProperty(this, '__index', { value: index });
        Object.defineProperty(this, '__which', { value: which });
    }
    __get(name) { return __native.animationLayer(this.__kind, this.__index, this.__which, name); }
    __set(name, v) { __native.animationLayer(this.__kind, this.__index, this.__which, 'set' + name, v); }
    get fps() { return this.__get('fps'); }
    get frameCount() { return this.__get('frameCount'); }
    get duration() { return this.__get('duration'); }
    get name() { return this.__get('name'); }
    get rate() { return this.__get('rate'); }
    set rate(v) { this.__set('Rate', __num(v)); }
    get blend() { return this.__get('blend'); }
    set blend(v) { this.__set('Blend', __num(v)); }
    get visible() { return this.__get('visible'); }
    set visible(v) { this.__set('Visible', !!v); }
    play() { this.__set('Playing', true); }
    pause() { this.__set('Playing', false); }
    stop() { this.__set('Stop', true); }
    isPlaying() { return this.__get('playing'); }
    getFrame() { return this.__get('frame'); }
    setFrame(f) { this.__set('Frame', __num(f)); }
    addEndedCallback(f) { }
}

class EffectRef {
    constructor(kind, index, which) {
        Object.defineProperty(this, '__kind', { value: kind });
        Object.defineProperty(this, '__index', { value: index });
        Object.defineProperty(this, '__which', { value: which });
    }
    get visible() { return !!__native.effect(this.__kind, this.__index, this.__which, 'visible'); }
    set visible(v) { __native.effect(this.__kind, this.__index, this.__which, 'setVisible', !!v); }
    get name() { return __native.effect(this.__kind, this.__index, this.__which, 'name'); }
    getMaterialCount() { return __native.effect(this.__kind, this.__index, this.__which, 'materialCount'); }
    getMaterial(i) { return { getAnimation() { return new NoAnimation(); } }; }
    setMaterialProperty(name, value) { __native.effect(this.__kind, this.__index, this.__which, 'setProperty:' + name, __unwrap(name, value)); }
    executeMaterialFunction(name) { }
    getAnimation(name) {
        const key = __native.effect(this.__kind, this.__index, this.__which, 'animationKey:' + (name === undefined ? '' : name));
        return key >= 0 ? new Animation(key) : new NoAnimation();
    }
}

const __timers = [];
let __now = 0;
const engine = {
    AUDIO_RESOLUTION_16: 16,
    AUDIO_RESOLUTION_32: 32,
    AUDIO_RESOLUTION_64: 64,
    frametime: 0,
    runtime: 0,
    timeOfDay: 0,
    screenResolution: new Vec2(1920, 1080),
    canvasSize: new Vec2(1920, 1080),
    userProperties: {},
    isRunningInEditor() { return false; },
    isPortrait() { return this.screenResolution.y > this.screenResolution.x; },
    isLandscape() { return this.screenResolution.x >= this.screenResolution.y; },
    isDesktopDevice() { return true; },
    isMobileDevice() { return false; },
    isWallpaper() { return true; },
    isScreensaver() { return false; },
    registerAudioBuffers(resolution) {
        const n = resolution === 32 ? 32 : resolution === 64 ? 64 : 16;
        const buffers = { left: new Float32Array(n), right: new Float32Array(n), average: new Float32Array(n) };
        __glint.audio.push(buffers);
        return buffers;
    },
    registerAsset(file, precache) { return { path: String(file) }; },
    setTimeout(callback, delay) {
        const t = { at: __now + (delay || 0), callback, repeat: 0, dead: false };
        __timers.push(t);
        return () => { t.dead = true; };
    },
    setInterval(callback, delay) {
        const t = { at: __now + (delay || 0), callback, repeat: Math.max(1, delay || 0), dead: false };
        __timers.push(t);
        return () => { t.dead = true; };
    },
    openUserShortcut(name) { return false; },
};
const input = {
    cursorWorldPosition: new Vec3(0, 0, 0),
    cursorScreenPosition: new Vec2(0, 0),
    cursorLeftDown: false,
};
const console = {
    log(...args) { __native.log(0, args.map(a => typeof a === 'object' ? (a === null ? 'null' : (a instanceof Vec2 || a instanceof Vec3 ? a.toString() : JSON.stringify(a))) : String(a)).join(' ')); },
    error(...args) { __native.log(1, args.map(a => String(a)).join(' ')); },
    warn(...args) { __native.log(1, args.map(a => String(a)).join(' ')); },
    info(...args) { __native.log(0, args.map(a => String(a)).join(' ')); },
    debug(...args) { },
};
const shared = {};
const __storage = { screen: new Map(), global: new Map() };
const localStorage = {
    LOCATION_GLOBAL: 'global',
    LOCATION_SCREEN: 'screen',
    set(key, value, location) { (__storage[location === 'global' ? 'global' : 'screen']).set(String(key), value); __native.store(location === 'global' ? 'global' : 'screen', String(key), JSON.stringify(value === undefined ? null : value)); },
    get(key, location) { return (__storage[location === 'global' ? 'global' : 'screen']).get(String(key)); },
    delete(key, location) { const m = __storage[location === 'global' ? 'global' : 'screen']; const had = m.delete(String(key)); __native.store(location === 'global' ? 'global' : 'screen', String(key), undefined); return had; },
    remove(key, location) { return this.delete(key, location); },
    clear(location) { (__storage[location === 'global' ? 'global' : 'screen']).clear(); },
};
const thisScene = {
    getLayer(which) { const p = __native.findLayer(which); return p ? __glint.layer(p[0], p[1]) : undefined; },
    getLayerByID(id) { const p = __native.findLayer(Number(id)); return p ? __glint.layer(p[0], p[1]) : undefined; },
    getLayerCount() { return __native.layerCount(); },
    enumerateLayers() { const out = []; const n = __native.layerCount(); for (let i = 0; i < n; ++i) { const p = __native.layerAt(i); if (p) out.push(__glint.layer(p[0], p[1])); } return out; },
    destroyLayer(l) { return false; },
    createLayer(c) { __native.warn('createLayer is not supported'); return undefined; },
    sortLayer(l, i) { return false; },
    getLayerIndex(l) { const p = typeof l === 'object' && l !== null ? [l.__kind, l.__index] : __native.findLayer(l); return p ? __native.layerIndex(p[0], p[1]) : -1; },
    getInitialLayerConfig(l) { return {}; },
    getCameraTransforms() { return { eye: new Vec3(0, 0, 0), center: new Vec3(0, 0, -1), up: new Vec3(0, 1, 0), zoom: 1 }; },
    setCameraTransforms(t) { },
    getAnimation(name) { return new NoAnimation(); },
    createModelData(c) { return {}; },
    destroyModelData(m) { },
};
globalThis.engine = engine;
globalThis.input = input;
globalThis.console = console;
globalThis.shared = shared;
globalThis.localStorage = localStorage;
globalThis.thisScene = thisScene;
globalThis.renderContext = {};

const __layers = new Map();
const __glint = {
    audio: [],
    scripts: [],
    layer(kind, index) {
        const key = kind * 1000003 + index;
        let l = __layers.get(key);
        if (!l) { l = new Layer(kind, index); __layers.set(key, l); }
        return l;
    },
    effect(kind, index, which) { return __native.effect(kind, index, which, 'exists') ? new EffectRef(kind, index, which) : undefined; },
    animationLayer(kind, index, which) { return __native.animationLayer(kind, index, which, 'exists') ? new AnimationLayerRef(kind, index, which) : undefined; },
    // thisObject for a script: the layer, or the effect the property belongs to.
    object(script) {
        const s = __glint.scripts[script];
        if (s.effect !== undefined) {
            const e = new EffectRef(s.kind, s.index, s.effect);
            e.getAnimation = (name) => name === undefined ? __glint.scriptAnimation(script) : EffectRef.prototype.getAnimation.call(e, name);
            return e;
        }
        // The layer, with getAnimation() meaning the timeline of this script's own field.
        const layer = __glint.layer(s.kind, s.index);
        const o = Object.create(layer);
        o.getAnimation = (name) => name === undefined ? __glint.scriptAnimation(script) : layer.getAnimation(name);
        return o;
    },
    scriptAnimation(script) {
        const key = __native.scriptAnimationKey(script);
        return key >= 0 ? new Animation(key) : new NoAnimation();
    },
    // createScriptProperties for a script: the builder's defaults under the stored values.
    properties(script) {
        return function createScriptProperties() {
            const defaults = {};
            const builder = {
                __add(def) { if (def && def.name !== undefined) defaults[def.name] = def.value; return builder; },
                addSlider(d) { return builder.__add(d); },
                addCheckbox(d) { return builder.__add(d); },
                addCombo(d) { if (d && d.value === undefined && d.options && d.options.length) d.value = d.options[0].value; return builder.__add(d); },
                addText(d) { return builder.__add(d); },
                addColor(d) { if (d && typeof d.value === 'string') d.value = new Vec3(d.value); return builder.__add(d); },
                addTask(d) { return builder; },
                finish() {
                    const stored = __native.scriptProperties(script) || {};
                    const out = {};
                    for (const k in defaults) out[k] = defaults[k];
                    for (const k in stored) {
                        const v = stored[k];
                        out[k] = defaults[k] instanceof Vec3 && typeof v === 'string' ? new Vec3(v) : v;
                    }
                    return out;
                },
            };
            return builder;
        };
    },
    // Timers due by now, in order.
    tick(nowMs) {
        __now = nowMs;
        for (let i = 0; i < __timers.length; ++i) {
            const t = __timers[i];
            if (t.dead) continue;
            if (t.at > __now) continue;
            if (t.repeat > 0) t.at += t.repeat; else t.dead = true;
            try { t.callback(); } catch (e) { __native.log(1, 'timer: ' + (e && e.stack ? e.stack : e)); }
        }
        for (let i = __timers.length - 1; i >= 0; --i) if (__timers[i].dead) __timers.splice(i, 1);
    },
    // The 64-band spectrum into every registered buffer: means of neighbouring bands.
    audioTick(left, right) {
        for (const b of __glint.audio) {
            const n = b.left.length, group = 64 / n;
            for (let k = 0; k < n; ++k) {
                let l = 0, r = 0;
                for (let g = 0; g < group; ++g) { l += left[k * group + g]; r += right[k * group + g]; }
                b.left[k] = l / group;
                b.right[k] = r / group;
                b.average[k] = (b.left[k] + b.right[k]) * 0.5;
            }
        }
    },
    // The value a script's update() gets and the one it returns, across the bridge.
    toScript: __toScript,
    toScene: __toScene,
    wrap: __wrap,
    unwrap: __unwrap,
    // User properties as stored, colours as Vec3 (the docs promise that much).
    userProperties(values, colors) {
        const out = {};
        for (const k in values) out[k] = colors.indexOf(k) >= 0 && typeof values[k] === 'string' ? new Vec3(values[k]) : values[k];
        return out;
    },
    register(script, kind, index, effect, field) {
        __glint.scripts[script] = { kind, index, effect: effect < 0 ? undefined : effect, field, ns: {} };
    },
    setNamespace(script, ns) { __glint.scripts[script].ns = ns; },
    loadStorage(obj) {
        for (const loc of ['screen', 'global']) {
            const m = obj && obj[loc];
            if (m && typeof m === 'object') for (const k in m) __storage[loc].set(k, m[k]);
        }
    },
    // One of a script's exported functions with the field's value, and what it returns for
    // the scene; undefined leaves the field alone.
    run(script, name, raw) {
        const s = __glint.scripts[script];
        const fn = s.ns[name];
        if (typeof fn !== 'function') return undefined;
        const v = __toScript(s.field, raw);
        const r = fn(v);
        return r === undefined ? undefined : __toScene(s.field, r, v);
    },
    // An event handler, if the script exports it.
    event(script, name, arg) {
        const s = __glint.scripts[script];
        const fn = s.ns[name];
        if (typeof fn !== 'function') return false;
        fn(arg);
        return true;
    },
    has(script, name) { return typeof __glint.scripts[script].ns[name] === 'function'; },
    cursorEvent(world, local) { const e = new CursorEvent(); e.worldPosition = new Vec3(world[0], world[1], 0); e.localPosition = new Vec3(local[0], local[1], 0); return e; },
};
globalThis.__glint = __glint;
)JS";

}  // namespace glint::scene
