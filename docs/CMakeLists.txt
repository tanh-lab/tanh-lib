# Documentation: Doxygen (XML) -> Breathe -> Sphinx (HTML), the same pipeline
# anira uses. Opt-in through TANH_WITH_DOCS; nothing here is part of ALL, so
# a docs-enabled configure only costs a `find_package(Doxygen)`.
#
#   cmake -S . -B build/docs -DTANH_WITH_DOCS=ON
#   cmake --build build/docs --target sphinx-docs     # -> build/docs/docs/sphinx/html
#
# Sphinx and its extensions are installed into a virtual environment inside the
# build tree, so the only host dependencies are Python 3, Doxygen and (for the
# inheritance diagrams) Graphviz.

# Only the interpreter is needed (to create the venv); requesting the Development
# component would force CMake to pick a Python that also ships libpython headers,
# which can select a stray/self-built interpreter with a broken ensurepip.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_package(Doxygen REQUIRED)

set(DOCS_VENV_DIR "${CMAKE_CURRENT_BINARY_DIR}/venv")

# ---- Sphinx virtual environment ---------------------------------------------
add_custom_target(create_docs_venv)

if(NOT EXISTS ${DOCS_VENV_DIR})
    add_custom_command(
        TARGET create_docs_venv
        POST_BUILD
        COMMAND ${Python3_EXECUTABLE} -m venv ${DOCS_VENV_DIR}
        COMMAND ${DOCS_VENV_DIR}/bin/pip install --quiet --upgrade pip
        COMMAND ${DOCS_VENV_DIR}/bin/pip install --quiet sphinx breathe shibuya myst-parser
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Creating virtual environment for Sphinx documentation"
        VERBATIM
    )
endif()

# ---- Doxygen -----------------------------------------------------------------
set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/doxygen/Doxyfile.in)
set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/doxygen/Doxyfile)
configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)

if(NOT DOXYGEN_DOT_FOUND)
    message(STATUS "Graphviz (dot) not found: the API reference is built without inheritance diagrams")
endif()

# Not ALL: the docs must never build alongside the library.
add_custom_target(doxygen-docs
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
)

# ---- Breathe: one .rst page per class / struct --------------------------------
set(SPHINX_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sphinx)
set(SPHINX_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/sphinx)

# breathe-apidoc writes into the Sphinx source tree (docs/sphinx/api/class,
# docs/sphinx/api/struct and the two *list.rst indexes); those paths are
# gitignored and regenerated on every docs build.
add_custom_target(breathe-apidocs
    COMMAND ${DOCS_VENV_DIR}/bin/breathe-apidoc -o ${SPHINX_SOURCE_DIR}/api
            ${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml
            -g class,struct -f -q
    COMMAND ${DOCS_VENV_DIR}/bin/python ${CMAKE_CURRENT_SOURCE_DIR}/prune-nested-pages.py ${SPHINX_SOURCE_DIR}/api
    # Ask Breathe to render the Doxygen inheritance/collaboration graphs.
    # perl -i rather than sed -i: BSD sed on macOS spells the flag differently.
    COMMAND bash -c "find ${SPHINX_SOURCE_DIR}/api/class -name '*.rst' -exec perl -i -pe 's{^(\\.\\.\\s+doxygenclass::.*\\n)}{$1    :allow-dot-graphs:\\n}' {} +"
    COMMAND bash -c "find ${SPHINX_SOURCE_DIR}/api/struct -name '*.rst' -exec perl -i -pe 's{^(\\.\\.\\s+doxygenstruct::.*\\n)}{$1    :allow-dot-graphs:\\n}' {} +"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating per-class API pages with breathe-apidoc"
    DEPENDS create_docs_venv doxygen-docs
    VERBATIM
)

# ---- Sphinx --------------------------------------------------------------------
configure_file(${SPHINX_SOURCE_DIR}/conf.py.in ${SPHINX_BUILD_DIR}/conf.py @ONLY)

# Hand-written sources only: the breathe-apidoc output under api/ is regenerated
# (and pruned) on every build, so it must not be a file-level dependency.
file(GLOB_RECURSE SPHINX_SOURCE_FILES LIST_DIRECTORIES false "${SPHINX_SOURCE_DIR}/*")
list(FILTER SPHINX_SOURCE_FILES EXCLUDE REGEX "/api/(class|struct)/")
list(FILTER SPHINX_SOURCE_FILES EXCLUDE REGEX "/api/(class|struct)list\\.rst$")

add_custom_target(sphinx-docs
    COMMAND ${DOCS_VENV_DIR}/bin/sphinx-build -b html
            -c ${SPHINX_BUILD_DIR}
            ${SPHINX_SOURCE_DIR}
            ${SPHINX_BUILD_DIR}/html
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating HTML documentation with Sphinx"
    DEPENDS create_docs_venv doxygen-docs breathe-apidocs ${SPHINX_SOURCE_FILES}
    VERBATIM
)

if(TANH_WITH_INSTALL)
    install(DIRECTORY ${SPHINX_BUILD_DIR}/html/
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        COMPONENT documentation
        OPTIONAL
    )
endif()
