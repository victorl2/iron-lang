# Contributing Guide

Thank you for your interest in contributing! This project values quality, accountability, and genuine understanding over contribution volume. Please read this guide carefully before submitting a pull request.

---

## Video Walkthrough Requirement

**Every pull request must be accompanied by a screen recording walkthrough.**

This is not optional. PRs submitted without a video will not be reviewed.

### What the video must cover

- A walkthrough of the diff — what changed and where
- An explanation of *why* the change was made
- How the change works, in your own words
- Edge cases you considered and how you handled them (or consciously decided not to)
- Any tradeoffs or limitations you are aware of

### Format

- Screen recording with your voice narration
- **You must use your own natural voice.** AI-generated voices, text-to-speech tools, voice changers, or any other voice modification software are not permitted
- Any language is accepted — **English is preferred** if you are comfortable with it, but never required
- There is no minimum or maximum length — cover what needs to be covered
- Use any screen capture software you prefer — [Loom](https://loom.com), OBS, or similar tools all work fine
- **Upload the video to YouTube as an unlisted video** and link it directly in the PR description. Unlisted means it will not appear in search results but will be accessible to anyone with the link

### Content consent

By submitting a video walkthrough, you agree that **if your PR is merged, the video will be downloaded and permanently archived as part of the project's official history**. This content may be used for documentation, onboarding, and historical reference purposes. If you are not comfortable with this, please do not submit a contribution.

### Why we require this

We ask every contributor to demonstrate that they understand and own what they are submitting. This creates a verifiable, persistent track record of contributions and shifts the burden of proof where it belongs — on the contributor, not the maintainers.

It also means that how you wrote the code — with or without AI assistance — is not our concern. What matters is that you can explain it, defend it, and stand behind it.

---

## Exemptions

The video requirement may be waived for:

- Documentation-only changes (typos, wording fixes, README updates)
- Dependency version bumps with no behavioral changes
- Formatting or whitespace-only commits
- **Recurring contributors with an established track record**, when submitting small, well-scoped changes — maintainers may grant this at their discretion based on the contributor's history of prior video walkthroughs

If you believe your change qualifies for an exemption, state so explicitly in the PR description. Maintainers have final say.

---

## Automated Enforcement

This project uses automated bots to assist maintainers in enforcing contribution requirements. When you open a pull request, the following checks will run automatically:

- **Video link detection** — the bot will verify that a valid YouTube link is present in the PR description. PRs without one will be automatically flagged as not ready for review
- **Video accessibility check** — the bot will confirm the video is publicly accessible. A private or deleted video will trigger the same flag
- **Transcript extraction** — once a valid link is detected, the bot will automatically fetch the YouTube transcript and attach it as a comment on the PR. This makes your explanation readable and searchable without requiring maintainers to watch the video
- **PR labeling** — PRs will be automatically labeled as `needs-video`, `video-provided`, or `exempt` based on their status, keeping the review queue organized

These checks are not a substitute for human review — they are a first pass to ensure contributions meet the baseline requirements before a maintainer's time is spent on them.

---

Before submitting, make sure you can answer yes to all of the following:

- [ ] I have watched my own video and it clearly explains the change
- [ ] My code is tested and the tests pass
- [ ] I have considered edge cases and documented any known limitations
- [ ] I understand every line I am submitting and can answer questions about it
- [ ] I have the right to submit this code under this project's license

---

## Code of Conduct

By contributing, you attest that the code you submit is your own responsibility. You understand it, you own it, and you are prepared to discuss it.

---

## A Note on AI-Assisted Development

We do not ban or restrict the use of AI tools. We do require that every contributor understands and can explain what they submit. If you used AI to help write your code, that is fine — as long as your video walkthrough demonstrates genuine understanding. A convincing explanation is the only bar that matters.

---

*This policy exists to build a project where every contribution has a human being behind it who stands accountable for their work. We believe that standard is good for the project, good for contributors, and good for the long-term health of the codebase.*
