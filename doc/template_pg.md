# COMPONENT_NAME Programmer's Guide {#COMPONENT_NAME_pg}

# In this document {#COMPONENT_NAME_pg_toc}

* @ref COMPONENT_NAME_pg_audience
* @ref COMPONENT_NAME_pg_intro
* @ref COMPONENT_NAME_pg_theory
* @ref COMPONENT_NAME_pg_design
* @ref COMPONENT_NAME_pg_examples
* @ref COMPONENT_NAME_pg_sequences
* @ref COMPONENT_NAME_pg_implementation

## Target Audience {#COMPONENT_NAME_pg_audience}

Explain that the programmer's guide is a supplement to the API documentation, providing both context and architectural
insights. If your guide has some other unique aspects to it, call them out here so the reader knows what to expect.
Mention that the guide is not intended to serve as a design document or an API reference and also some sort of reference
to the source code with a direct link to github.

## Introduction {#COMPONENT_NAME_pg_intro}

Provide some high level description of what this component is, what it does and why it exists. This shouldn't be
a lengthy tutorial or commentary on storage in general or the goodness of SPDK but provide enough information to
set the stage for someone about to write an application to integrate with this component.  It is safe to assume that the
reader of this guide is already familiar with the basic principles behind SPDK (kernel bypass, polling, message passing,
etc.) but don't shy away from short descriptions of general concepts that are critical to understanding how this
component works.

## Theory of Operation {#COMPONENT_NAME_pg_theory}

Create subsections here to drill down into the how this component works. This isn't a design section however so
avoid getting into too many details, just hit the high level concepts that would leave the developer with a
50K foot overview of the major elements/assumptions/concepts that should have some baseline knowledge about before
they start writing code. A good test is to ask yourself if content in this section explains all of the concepts
presented in the public API for this component. If an implementation detail doesn't have a material impact on the
use of the component, don't describe it. Be sure to explain relevant configuration details for this module as they
may differ from other components in SPDK.

### Subsections

If your guide has subsections to any of the main topics, use the formatting shown here to break out topics.

## Design Considerations {#COMPONENT_NAME_pg_design}

Here is where you want to highlight things they need to think about in *their* design. If you have written test code
for this module think about the things that you needed to go learn about to properly interact with this module. Think
about how they need to consider initialization options, threading, limitations, any sort of quirky or non-obious
interactions or module behaviors that might save them some time and effort by thinking about before they start their
design.

## Examples {#COMPONENT_NAME_pg_examples}

List all of the relevant examples we have in the repo that use this module and describe a little about what they do.

## Sequences {#COMPONENT_NAME_pg_sequences}

This is an optional section, if the module being covered has complex enough public API that are better explained
graphically then use sequence diagrams. Using mscgen create simple UML-style (don't need to be 100% UML compliant)
sequence diagrams for the API that an application needs to interact with.  Details within the component should not
be included. If you find yourself writing diagrams that are super simple, just don't include this ssection.

## Implementation Details {#COMPONENT_NAME_pg_implementation}

This is where you can provide some design level detail if you believe it will help the developer be able to more
effectively interact with the module. We don't want to have design docs as part of SPDK, the overhead and maintenance
is too much for open source. We do, however, want to provide some level of insight into the codebase to promote
getting more people involved and understanding of what the design is all about.  The PG is meant to help a developer
write their own application but you can use this section, per module, to test out a way to build out some internal
design info as well. An example mught be including an overview of key structures, concepts, etc., of the module itself.
So, intersting info not required to write an application using the module but maybe just enough to provide the next level of
detail into what's behind the scenes to get someone more intertested in becoming a cmmunity contributor.
