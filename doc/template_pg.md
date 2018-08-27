# ComponentName Programmer's Guide {#componentname_pg}

# In this document {#componentname_pg_toc}

@ref componentname_pg_audience
@ref componentname_pg_intro
@ref componentname_pg_theory
@ref componentname_pg_design
@ref componentname_pg_examples
@ref componentname_pg_config
@ref componentname_pg_component
@ref componentname_pg_sequences

## Target Audience {#componentname_pg_audience}

This programmer's guide is intended for developers authoring applications that utilize the SPDK <COMPONENT NAME>. It is
intended to supplement the source code to provide an overall understanding of how to integrate <COMPONENT NAME> into
an application as well as provide some high level insight into how <COMPONENT NAME> works behind the scenes. It is not
intended to serve as a design document or an API reference but in some cases source code snippets and high level
sequences will be discussed. For the latest source code reference refer to the [repo](https://github.com/spdk).

## Introduction {#componentname_pg_intro}

Provide some high level description of what this component is, what it does and maybe why it exists. This shouldn't be
a lengthy tutorial or commentary on storage in general or the goodness of SPDK but provide enough information to
set the stage for someone about to write an application to integrate with this component.  They won't be totally
starting from scratch if they're at this point, they are by definition a storage application developer if they are
reading this guide.

## Theory of Operation {#componentname_pg_theory}

Create subsections here to drill down into the "how" this component works. This isn't a design section however so
avoid getting into too many details, just hit the high level concepts that would leave the developer with a
50K foot overview of the major elements/assumptions/concepts that should have some baseline knowledge about before
they start writing code.

Some questions to consider when authoring this section:

* What are the basic primitives that this component exposes?
* How are these primitives related to one another?
* What are the threading rules when using these primitives?
* What are the theoretical performance implications for different scaling vectors?
* Are there any other documents or specifications that the user should be familiar with?
* What are the intended use cases?

## Design Considerations {#componentname_pg_design}

Here is where you want to highlight things they need to think about in *their* design. If you have written test code
for this module think about the things that you needed to go learn about to properly interact with this module. Think
about how they need to consider initialization options, threading, limitations, any sort of quirky or non-obious
interactions or module behaviors that might save them some time and effort by thinking about before they start their
design.

## Examples {#componentname_pg_examples}

List all of the relevant examples we have in the repo that use this module and describe a little about what they do.

## Configuration {#componentname_pg_config}

This section should describe the mechanisms for configuring the component at a high level (i.e. you can configure it
using a config file, or you can configure it using RPC calls over a unix domain socket). It should also talk about
when you can configure it - i.e. at run time or only up front. For specifics about how the RPCs work or the config
file format, link to the appropriate user guide instead of putting that information here.

## Component Detail {#componentname_pg_component}

This is where we can provide some design level detail if it makes sense for this module. We don't want to have
design docs as part of SPDK, the overhead and maintenance is too much for open source. We do, however, want
to provide some level of insight into the codebase to promote getting more people involved and understanding
of what the design is all about.  The PG is meant to help a developer write their own application but we
can use this section, per module, to test out a way to build out some internal design info as well. I see
this as including an overview of key structures, concepts, etc., of the module itself. So, interesting info
not required to write an application using the module but maybe just enough to provide the next level of
detail into what's behind the scenes to get someone more interested in becoming a community contributor.

## Sequences {#componentname_pg_sequences}

If sequence diagrams makes sense for this module, use mscgen to create simple UML-style (they don't need to be 100%
UML compliant) diagrams for the API that an application needs to interact with.  Details internal to the component
should not be included.
