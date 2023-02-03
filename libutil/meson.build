mylibutil_src = files(['completion.c'])

mylibutil_dep = dependency_threads

mylibutil = library('libutil',
		    mylibutil_src,
		    dependencies : mylibutil_dep)

dependency_libutil = declare_dependency(link_with : mylibutil)
