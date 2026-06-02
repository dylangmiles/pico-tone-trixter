---
layout: page
title: Pico Tone Trixter
image:
  path: /assets/og/og-home.jpg
  width: 1200
  height: 630
---

**Can a $7 codec chip make a piezo-equipped acoustic guitar sound like it was recorded with a $300 studio microphone?**

<img class="hero-image" src="{{ '/episodes/2026-05-ir-killed-the-quack/assets/pedal-interior-daughter.jpg' | relative_url }}" alt="Inside the Tone Trixter — a Raspberry Pi Pico 2, ES8388 codec module and JFET daughter board mounted vertically inside a copper-tape-lined wooden gift box, with TRS jacks on opposite walls.">

This is the build log for a real-time guitar pedal that does exactly that — impulse-response convolution running on a Raspberry Pi Pico 2 (RP2350), turning the harsh, nasal "quack" of an under-saddle pickup into the warm, miked sound of the guitar's body. Low latency, battery-powered, built on the bench in Cape Town.

## Build log

<ul class="episode-list">
{%- assign episodes = site.episodes | sort: "date" | reverse -%}
{%- for ep in episodes %}
  <li>
    <a href="{{ ep.url | relative_url }}"><strong>{{ ep.title }}</strong></a><br>
    <small>Episode {{ ep.episode }} · {{ ep.date | date: "%B %Y" }}</small>
    {%- if ep.subtitle %}<br><em>{{ ep.subtitle }}</em>{% endif %}
  </li>
{%- endfor %}
</ul>

---

Source, schematics, and firmware: [github.com/dylangmiles/pico-tone-trixter](https://github.com/dylangmiles/pico-tone-trixter)