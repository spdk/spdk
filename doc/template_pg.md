---
layout: default
title:  "SPDK <COMPONENT NAME> Programmer's Guide"
---

# In this document:

* [Target Audience](#audience)
* [Introduction](#intro)
* [Theory of Operation](#theory)
* [Design Considerations](#design)
* [Examples](#examples)
* [Configuration](#config)
* [Component Detail](#component)
* [Sequences](#sequences)

<a id="audience"></a>
## Target Audience

The programmer's guide is intended for developers authoring applications that utilize the SPDK <COMPONENT NAME>. It is
intended to supplement the source code in providing an overall understanding of how to integrate <COMPONENT NAME> into
an application as well as provide some high level insight into how <COMPONENT NAME> works behind the scenes. It is not
intended to serve as a design document or an API reference and in some cases source code snippets and high level
sequences will be discussed; for the latest source code reference refer to the [repo](https://github.com/spdk).

<a id="intro"></a>
## Introduction

Provide some high level description of what this component is, what it does and maybe why it exists. This shouldn't be
a lengthy tutorial or commentary on storage in general or the goodness of SPDK but provide enough information to
set the stage for someone about to write an application to integrate with this component.  They won't be totally
starting from scratch if there at this point, they are by defintion a storage applicaiton developer if they are
reading this guide.

<a id="theory"></a>
## Theory of Operation

Create subsections here to drill down into the "how" this component works. This isn't a design section however so
avoid getting into too many details, just hit the high level concepts that would leave the developer with a
50K foot overview of the major elements/assumptions/concepts that should have some baseline knowledge about before
they start writing code.

<a id="considerations"></a>
## Design Considerations

Here is where you want to highlight things they need to think about in *their* design. If you have written test code
for this module think about the things that you needed to go learn about to properly interact with this module. Think
about how they need to consider initialization options, threading, limitations, any sort of quirky or non-obious
interactions or module behaviors that might save them some time and effort by thinking about before they start their
design.

<a id="examples"></a>
## Examples

List all of the relevant examples we have in the repo that use this module and describe a little about what they do.

<a id="config"></a>
## Configuration

This may be really short or could be very long, depends on the module.  Focus more on the different methods for
configuration and when/why one might use one over the other.  It's OK to call out some important ones that aren't
likley to change but avoid listing everything with every little detail here.  This isn't the cofiguration guide, this
is a section explaining to another developer what their options are for configuring this module and maybe what some
of the really important ones are.

<a id="sequences"></a>
## Sequences

<Still need to have some discussion over the right tool, stay tuned....>

Using mscgen create simple UML-style (don't need to be 100% UML compliant) sequence diagrams for the API
that an application needs to interact with.  Details within the component should not be included.

<a id="component"></a>
## Component Detail

This is where we can (up for discussion of course) provide some design level detail. We don't want to have
design docs as part of SPDK, the overhead and maintenance is too much for open source. We do, however, want
to provide some level of insight into the codebase to promote getting more people involved and understanding
of what the design is all about.  The PG is meant to help a developer write their own application but we
can use this section, per module, to test out a way to build out some internal design info as well. I see
this as including an overview of key structures, concepts, etc., of the module itself. So, intersting info
not required to write an application using the module but maybe just enough to provide the next level of
detail into what's behind the scenes to get someone more intertested in becoming a cmmunity contributor.
