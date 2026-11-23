# AI recaps (experimental)

This fork adds on-demand EPUB recaps using your own OpenAI or Anthropic API key.
Requests go directly from the reader to the selected provider over Wi-Fi. A
companion service is not required. The first hardware target is the Xteink X3;
hardware testing is required before treating the feature as ready for everyday
use.

## Reading behavior

Open **AI recap** from an EPUB's reader menu. The available scopes are:

- **Previous chapter:** the chapter before the current chapter, including any
  EPUB sections that CrossInk groups into that chapter.
- **Last pages:** a chosen number of displayed pages, ending with the current
  page. For example, ten pages means the current page and the preceding nine.
- **So far:** the beginning of the book through the current page, subject to the
  request-size limit.

The current page is treated as read, including its last line. Choose **Short**
for a brief recap or **Detailed** for a longer one. Both modes summarize factual
events or ideas; they are not intended to predict, interpret foreshadowing, or
answer open-ended questions about the book.

Screen pages depend on the current font, spacing, margins, and orientation.
They are not printed-edition page numbers. The source range is frozen when the
recap flow is opened. Opening a footnote preview does not authorize its target
location as the reading cutoff.

The cutoff assumes earlier pages have been read; it does not track every page
actually visited. Earlier front matter or a table of contents can be included
if it falls within the selected rendered pages. Books whose chapters are only
anchors inside a single EPUB section may need page-range mode instead of
previous-chapter mode. After changing fonts, allow current-section indexing to
finish before requesting a range that needs earlier sections rebuilt.

## Provider setup

Choose a provider, enter a compatible model identifier, and enter that provider's
API key in the reader's password-entry screen. Provider accounts must have API
access and sufficient credit or billing enabled. Consumer chat subscriptions
are separate from this firmware's API integration.

Keys are saved in the device's internal settings, not on its SD card. They are
not exposed through CrossInk's existing unauthenticated file-transfer website.
This does not provide protection against physical extraction of the device's
flash. Keep the reader under your control and revoke a key if the device is
lost. A separate key with a provider-side budget is useful for this feature.

Generation sends the selected passages and a bounded amount of earlier context
to the selected provider. The book title, author, full table of contents, and
unread sections are not included as additional context. The provider's data
handling terms apply to those requests.

## Spoiler boundary

The source collector reads the reader's rendered page text, one page at a time.
It does not upload the EPUB or ask a model to find the right chapter in a whole
book. It never extends the selected range beyond the frozen current page.
For a previous-chapter recap, the current chapter is also excluded from the
request's context.

Images are not interpreted. Text clipped outside a table's visible area is
excluded. These are text recaps, so diagrams and image-based passages may leave
gaps in the available context.

Earlier context is limited to 8 KiB and target passages to 64 KiB. If the target
does not fit, generation stops rather than silently dropping part of the target.
Choose a smaller page range in that case. Context limits must be visible in the
recap flow. This initial implementation does not maintain a whole-book AI memory.

The request instructs the model to use only the supplied passages, treat book
text as data rather than instructions, and avoid future facts, predictions,
interpretive hints, and outside knowledge. These instructions reduce risk but
cannot guarantee a spoiler-free response. A pretrained model may know the book,
infer later developments, or summarize misleadingly.

Saved recaps must match the exact source text, selected scope, reading boundary,
provider, model, detail level, and prompt version before reuse. A recap from a
later reading position must never appear as the result for an earlier position.
Saved recaps and their source evidence are local SD-card files, available for
offline reuse; they have the same physical-access exposure as books on the card.

## Network behavior

The reader releases the open book's working memory before the network operation
and returns to the saved reading position afterward. Wi-Fi is activated on demand.
The HTTPS request validates the server certificate and hostname. Redirects and
automatic request retries are disabled, so a failed request does not silently
repeat a potentially billable operation.

Responses are bounded and checked before display. Provider errors, incomplete
responses, and refusals are not shown as successful recaps. Cancellation can be
limited by an in-flight socket operation's timeout. Cancelling locally cannot
guarantee that a request already accepted by a provider will not be billed.

## Verification before release

Use a test endpoint or an instrumented test build to inspect source text without
sending a real book or real key to an untrusted service. On an X3, verify:

1. A marker on the current page is included, while a marker on the next page is
   absent, including at a chapter boundary and during incremental indexing.
2. Last-page ranges cross section boundaries correctly and stop at the book's
   beginning. A previous chapter spanning several EPUB files stays complete.
3. Changing fonts or orientation, following a forward footnote, and jumping ahead
   then back cannot expand the source cutoff or reuse a later recap.
4. A changed book, model, provider, scope, or detail level invalidates the relevant
   cached result. Interrupted cache writes cannot publish a partial recap.
5. Invalid keys, rate limits, a disconnected network, an invalid certificate,
   a redirect, an oversized response, and cancellation return readable errors.
6. The reader returns to the original page; saved recaps work offline; credentials
   are absent from SD files, logs, and browser responses.
7. Measure free heap, largest allocatable block, task-stack margin, battery use,
   and firmware size on hardware. A successful desktop test is insufficient.

Evaluate generated output separately with twist-heavy fiction and nonfiction.
Source-boundary tests cannot establish that model prose contains no hints.

The simulator can test the UI and local cache but deliberately does not contact
paid provider APIs. Test the real HTTPS flow separately on the X3 with a limited
API key and a short, non-sensitive fixture.

## Contribution and API references

CrossInk's README currently declines pull requests and directs major features to
CrossPoint. Establish maintainer interest and the intended upstream before
proposing this experimental feature for inclusion. Keep text selection,
provider transport, and the reading UI independently reviewable.

- [OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)
- [Anthropic Messages](https://platform.claude.com/docs/en/api/messages/create)
