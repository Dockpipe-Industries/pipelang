package markup

import (
	"errors"

	"github.com/Dockpipe-Industries/pipelang/src/lib/pipelang"
)

type origin struct {
	generated pipelang.Span
	authored  pipelang.Span
}

// originMap is local conversion data. It never changes compiler syntax or
// assigns XML offsets to the generated source used by the shared checker.
type originMap struct {
	entries        []origin
	authoredFile   pipelang.FileID
	generatedFile  pipelang.FileID
	authoredBytes  int
	generatedBytes int
	nodes          int
}

func buildOrigins(authoredFile *pipelang.SourceFile, authored *pipelang.ViewDeclaration,
	generatedFile *pipelang.SourceFile, generated *pipelang.ViewDeclaration) (originMap, bool) {
	if authored == nil || generated == nil || authored.Name != generated.Name ||
		len(authored.State) != len(generated.State) || len(authored.Events) != len(generated.Events) ||
		len(authored.Actions) != len(generated.Actions) || len(authored.State) > 128 ||
		len(authored.Events) > 128 || len(authored.Actions) > 128 {
		return originMap{}, false
	}
	origins := originMap{
		authoredFile:   authoredFile.ID,
		generatedFile:  generatedFile.ID,
		authoredBytes:  len(authoredFile.Text),
		generatedBytes: len(generatedFile.Text),
	}
	if !origins.add(generated.Span, authored.Span) {
		return originMap{}, false
	}
	for index, declared := range generated.State {
		original := authored.State[index]
		if declared.Name != original.Name || declared.Type != original.Type || !sameValue(declared.Value, original.Value) ||
			!origins.add(declared.Span, original.Span) || !origins.add(declared.Value.Span, original.Value.Span) {
			return originMap{}, false
		}
	}
	for index, declared := range generated.Events {
		original := authored.Events[index]
		if declared.Name != original.Name || declared.Payload != original.Payload || !origins.add(declared.Span, original.Span) {
			return originMap{}, false
		}
	}
	for index, declared := range generated.Actions {
		original := authored.Actions[index]
		if declared.Name != original.Name || declared.Payload != original.Payload ||
			declared.Operation != original.Operation || declared.Target != original.Target || !origins.add(declared.Span, original.Span) {
			return originMap{}, false
		}
	}
	if !origins.element(generated.Root, authored.Root, 1) {
		return originMap{}, false
	}
	return origins, true
}

func (origins *originMap) element(generated, authored pipelang.ViewElement, depth int) bool {
	origins.nodes++
	if depth > 8 || origins.nodes > 128 || generated.Component != authored.Component ||
		len(generated.Properties) != len(authored.Properties) || len(generated.Properties) > 4 ||
		len(generated.Children) != len(authored.Children) || !origins.add(generated.Span, authored.Span) {
		return false
	}
	for index, property := range generated.Properties {
		original := authored.Properties[index]
		if property.Name != original.Name || !sameValue(property.Value, original.Value) ||
			!origins.add(property.Span, original.Span) || !origins.add(property.Value.Span, original.Value.Span) {
			return false
		}
	}
	for index, child := range generated.Children {
		if !origins.element(child, authored.Children[index], depth+1) {
			return false
		}
	}
	return true
}

func sameValue(first, second pipelang.ViewValue) bool {
	return first.Kind == second.Kind && first.Text == second.Text &&
		first.Boolean == second.Boolean && first.Integer == second.Integer
}

func (origins *originMap) add(generated, authored pipelang.Span) bool {
	if !boundedSpan(generated, origins.generatedFile, origins.generatedBytes) ||
		!boundedSpan(authored, origins.authoredFile, origins.authoredBytes) ||
		generated.Start == generated.End || authored.Start == authored.End {
		return false
	}
	origins.entries = append(origins.entries, origin{generated: generated, authored: authored})
	return true
}

func boundedSpan(span pipelang.Span, file pipelang.FileID, length int) bool {
	return span.File == file && span.IsValid() && span.Start <= length && span.End <= length
}

func (origins originMap) authoredSpan(generated pipelang.Span) (pipelang.Span, bool) {
	if !boundedSpan(generated, origins.generatedFile, origins.generatedBytes) {
		return pipelang.Span{}, false
	}
	var selected *origin
	for index := range origins.entries {
		candidate := &origins.entries[index]
		if generated.Start < candidate.generated.Start || generated.End > candidate.generated.End ||
			generated.Start >= candidate.generated.End {
			continue
		}
		if selected == nil || candidate.generated.End-candidate.generated.Start < selected.generated.End-selected.generated.Start {
			selected = candidate
		}
	}
	if selected == nil {
		return pipelang.Span{}, false
	}
	return selected.authored, true
}

func (origins originMap) remapError(authored, generated *pipelang.SourceFile, cause error) (error, bool) {
	var original *pipelang.DiagnosticError
	if !errors.As(cause, &original) || original == nil || len(origins.entries) == 0 {
		return nil, false
	}
	diagnostics := append(pipelang.Diagnostics(nil), original.Diagnostics...)
	changed := false
	for index, diagnostic := range diagnostics {
		if diagnostic.Category != pipelang.CategorySemantic {
			continue
		}
		span, found := origins.authoredSpan(diagnostic.Primary)
		if !found {
			continue
		}
		diagnostics[index].Primary = span
		diagnostics[index].Related = append([]pipelang.RelatedSpan(nil), diagnostic.Related...)
		for relatedIndex, related := range diagnostic.Related {
			if span, found := origins.authoredSpan(related.Span); found {
				diagnostics[index].Related[relatedIndex].Span = span
			}
		}
		diagnostics[index].Related = append(diagnostics[index].Related, pipelang.RelatedSpan{
			Span:    diagnostic.Primary,
			Message: "Generated PipeLang location; XML origin identifies the containing authored start tag",
		})
		changed = true
	}
	if !changed {
		return nil, false
	}
	sources, ok := diagnosticSources(authored, generated, original.Sources)
	if !ok {
		return nil, false
	}
	mapped := &pipelang.DiagnosticError{Sources: sources, Diagnostics: diagnostics}
	return &mappedDiagnosticError{mapped: mapped, cause: cause}, true
}

func diagnosticSources(authored, generated *pipelang.SourceFile, existing *pipelang.SourceSet) (*pipelang.SourceSet, bool) {
	inputs := []pipelang.SourceInput{
		{Path: string(authored.ID), Data: []byte(authored.Text)},
		{Path: string(generated.ID), Data: []byte(generated.Text)},
	}
	for _, file := range existing.Files() {
		if file.ID != authored.ID && file.ID != generated.ID {
			inputs = append(inputs, pipelang.SourceInput{Path: string(file.ID), Data: []byte(file.Text)})
		}
	}
	sources, diagnostics := pipelang.NewSourceSet(inputs)
	if diagnostics.HasErrors() {
		return nil, false
	}
	_, hasAuthored := sources.File(authored.ID)
	_, hasGenerated := sources.File(generated.ID)
	return sources, hasAuthored && hasGenerated
}

// errors.As sees authored diagnostics first, while errors.Is and other typed
// error queries retain the original checker/refusal chain through Unwrap.
type mappedDiagnosticError struct {
	mapped *pipelang.DiagnosticError
	cause  error
}

func (failure *mappedDiagnosticError) Error() string {
	return failure.mapped.Error()
}

func (failure *mappedDiagnosticError) Unwrap() error {
	return failure.cause
}

func (failure *mappedDiagnosticError) As(target any) bool {
	diagnostic, ok := target.(**pipelang.DiagnosticError)
	if !ok {
		return false
	}
	*diagnostic = failure.mapped
	return true
}
