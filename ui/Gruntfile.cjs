module.exports = function(grunt) {

	const port = 8910;

	grunt.loadNpmTasks('grunt-contrib-connect');
	grunt.loadNpmTasks('grunt-contrib-qunit');

	grunt.initConfig({
		pkg: grunt.file.readJSON('package.json'),
		connect: {
			server: {
				options: {
					port: port,
					base: 'dist/test'
				}
			}
		},
		qunit: {
			all: {
				options: {
					urls: ['http://localhost:' + port]
				}
			}
		}
	});

	grunt.registerTask('default', ['connect', 'qunit']);
};
