$ SPDX-License-Identifier: LGPL-2.0-or-later
$set 1
1 Unknown
2 See backend assessment
3 Absent
4 bytes
5 Select a row to inspect both versions, proposed names and recovery information.
6 Source
7 Destination
8 Proposed action
9 Reason
10 Proposed name
12 Retained name
13 Failure state/domain/code
14 Journal directory
16 skipped
19 job
20 selected rows
21 matched
22 resolved
23 unresolved
24 Cancel or finish the current operation before comparing again.
25 Check byte bounds and dates. Use YYYY-MM-DD or an ISO UTC timestamp; order the bounds.
26 Cannot start operation
27 Select matching rows first.
28 Rule updated. Review rules before applying the changed plan.
29 Rule added. Review rules to see matched counts and consequences.
30 Rule removed. Review the changed rule list before execution.
31 For a reviewed rename, select one row and enter one basename.
32 Evaluating rules; no files have been changed.
33 Worker is busy; try the command again.
34 Revalidating and applying the reviewed plan...
35 Cancelling - finishing safe cleanup...
36 Resume
37 Pause
38 Paused
39  (pause requested)
40 completed
41  (totals incomplete)
42 File Manager - Synchronize Folders
43 Source folder:
44 Destination folder:
45 Compare only
46 Copy
47 Update: protect destination-newer
48 Copy missing only
49 Synchronize one way
50 Mirror - deletion review required
51 Operation:
52 Complete preservation
53 Ordinary mode/times; new caller ownership
54 Metadata:
55 Compare and rule review are available. Applying changes awaits native backend qualification.\nTwo-way sync, move and mirror execution require further qualification.
56 Minimum bytes:
57 Maximum bytes:
58 Modified on/after (local YYYY-MM-DD or UTC time):
59 Modified before (local YYYY-MM-DD or UTC time):
60 Compare folders
61 Path
62 Previous page
63 Next page
64 Matching class:
65 Action:
66 All matching in job
67 Selected matching rows
68 Scope:
69 Ask
70 Keep target
71 Keep both incoming
72 Tie / unknown fallback:
73 Do not replace nonempty with empty
74 Preserve replaced versions
75 Exact reviewed basename (one row):
76 Add / update matching rule
77 Remove selected rule
78 Choose folders and an operation, then compare. Review matching rules before applying changes.
79 Review rules
80 Apply reviewed plan
81 Cancel operation
82 Close
83 item
84 subtree
85 Select exactly one item or subtree root.
86 Folders and options
87 Comparison and results
88 Matching rules
89 This item
90 Matching in selected subtree
91 qualified
92 unsupported
93 not yet qualified
94 precision
95 metadata
96 Choose a folder
97 Browse...
98 Operation needs attention. See details and recovery information.
99 Comparison reads file contents. Byte and date bounds limit selected files.\nUnknown inputs block unsafe actions.
100 Ask / defer
101 Use source / replace target
102 Keep target / skip existing
103 Use newest
104 Strict source-newer
105 Source-newer or size-different at equal time
106 Replace if size differs
107 Prefer larger
108 Prefer smaller
109 Keep both: rename incoming
110 Keep both: rename existing
111 Rename incoming manually
112 Compare content
113 Merge folders
114 Retry matching failures
115 Cancel
116 Skip matching
117 Keep Left
118 Keep Right
119 Keep both
120 Restore deleted version
121 Keep edited version
122 Delete edited version
130 Existing regular files
131 Both edited
132 Deletion versus edit
133 Folder deletion versus edited child
134 Different object types
135 Different link types
136 Name collision
137 Required metadata differs
138 Unavailable object
139 Stale object
140 Ambiguous publication
141 Existing folders
142 Hard-link effects
143 Access failure
144 Space or quota failure
150 Scanning folders...
151 Review proposed changes
152 Revalidating reviewed objects...
153 Applying reviewed changes...
154 Completed
155 Failed
156 Cancelled
157 Partially completed
158 Recovery required
170 Review
171 Skip
172 Copy
173 Create folder
174 Merge folder
175 Replace
176 Remove file
177 Remove folder
190 Unknown object
191 Incomplete inventory
192 Identity unavailable
193 Size unavailable
194 Timestamp unavailable
195 Timestamp precision unavailable
196 Digest unavailable
197 Digest incomplete
198 Content changed during read
199 Digest identity mismatch
200 Source absent
201 Destination absent
202 Same size; content not verified
203 Same size and time; content not verified
204 Verified same content
205 Source newer
206 Destination newer
207 Different size
208 Different content
209 Different type
210 Unsupported object type
211 Folder pair
212 Excluded
213 Filtered by policy
214 Filter cannot be evaluated
215 Protected descendant
216 Name collision
217 Existing destination
218 Equal-time conflict
219 Ancestor conflict
220 Same object
221 Not reviewed
222 Folder path is too long.
223 Not enough memory.
224 Enter a profile name (1-64 bytes).
225 Profiles save job-wide rules. Item, selection and subtree decisions stay with this comparison.
226 Saving profile preferences...
227 Profile save was not queued; check the name or wait for the current save.
228 Choose an available saved profile.
229 This profile has options or dates this window cannot represent.
230 Profile loaded. Compare and review again before applying changes.
231 Profile preferences ready.
232 Profile name:
233 Save preferences
234 Loading saved preferences...
235 Load selected preferences
236 Saved preferences are unavailable.
237 Pending
238 Skipped
239 Completed
240 Failed
241 Cancelled
242 Recovery required
243 Item result
244 Journal name
245 Staging name
246 Operation journal identifier
247 Retained-target journal identifier
248 Yes
250 Deleting saved preferences...
251 Profile deletion was not queued; wait for the current preferences operation.
252 Delete saved profile '%s'?\nThis removes preferences only.
253 Delete profile
254 Incoming object
255 Existing target
256 Name applies to:
257 Finalizing the operation and releasing native resources...
258 Applying changes is unavailable until native execution is qualified.
259 Objects %llu-%llu of %llu
260 Preparing a fresh comparison...
261 Folder options changed. Compare again before reviewing or applying changes.
262 precision unavailable
263 File
264 Folder
265 Symbolic link
266 Junction
267 Reparse point
268 FIFO
269 Socket
270 Character device
271 Block device
272 Other object
273 Compare and review the complete current plan before applying changes.
274 Plan: %llu copies, %llu new folders, %llu replacements, %llu removals\n%llu skipped/protected, %llu to review/report; %llu bytes%s
275 Plan totals are not yet available.
276 Unavailable
277 Action
278 Result
279 Choose an action available for this matching class and scope.
280 Choose two folders and an operation, then Compare. Inspect the plan totals\nand select rows to see both versions and the proposed names.\n\nUse matching rules for one item, selected rows, a subtree or the whole job.\nReview rules after every edit. Saved profiles store preferences only;\nloading one always requires a fresh comparison and review.\n\nDisplayed \\xNN escapes retain original folder bytes until the field is edited.\nDate-only bounds use local midnight; UTC timestamps preserve exact bounds.\nPause and Cancel are requests. Closing waits for safe worker shutdown.
281 Help
282 Cannot read the selected folder name.
