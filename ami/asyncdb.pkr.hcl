packer {
	required_plugins {
		amazon = {
			version = ">= 1.3.0"
			source = "github.com/hashicorp/amazon"
		}
	}
}

variable "region" {
	type = string
	default = "eu-west-2"
	description = "An AMI is regional, so this has to be the region the stack deploys into."
}

variable "instance_type" {
	type = string
	default = "t3.micro"
	description = "The type the stack runs, so what is installed is installed on the hardware it will meet."
}

variable "subnet_id" {
	type = string
	default = ""
	description = "The subnet to bake in, or empty for the account's default VPC. Either way it has to reach the internet, and either way it is not this stack's network."
}

variable "ssh_cidrs" {
	type = list(string)
	default = ["0.0.0.0/0"]
	description = "Who may reach the temporary security group's port 22. Packer's own default, and the one thing about the builder worth narrowing."
}

variable "run_id" {
	type = string
	default = "local"
	description = "What this bake is, tagged onto the builder so that the run that started it is the only run that can sweep it up."
}

variable "commit" {
	type = string
	default = "unknown"
	description = "The commit the scripts came from, tagged onto the image because nothing else records what it was built from."
}

data "amazon-parameterstore" "base" {
	name = "/aws/service/ecs/optimized-ami/amazon-linux-2023/recommended/image_id"
	region = var.region
}

locals {
	timestamp = regex_replace(timestamp(), "[- TZ:]", "")

	tags = {
		"asyncdb:commit" = var.commit
		"asyncdb:source-image" = data.amazon-parameterstore.base.value
	}
}

source "amazon-ebs" "database" {
	region = var.region
	source_ami = data.amazon-parameterstore.base.value
	instance_type = var.instance_type
	subnet_id = var.subnet_id
	associate_public_ip_address = true
	temporary_security_group_source_cidrs = var.ssh_cidrs
	ssh_username = "ec2-user"

	launch_block_device_mappings {
		device_name = "/dev/xvda"
		volume_size = 30
		volume_type = "gp3"
		delete_on_termination = true
	}

	ami_name = "asyncdb-database-${local.timestamp}"
	ami_description = "The database tier's boot dependencies, without the asyncdb image"
	run_tags = { Name = "asyncdb-database-ami-builder", "asyncdb:run" = var.run_id }
	tags = merge(local.tags, { Name = "asyncdb-database-${local.timestamp}", "asyncdb:role" = "database" })
	snapshot_tags = merge(local.tags, { Name = "asyncdb-database-${local.timestamp}", "asyncdb:role" = "database" })
}

source "amazon-ebs" "etcd" {
	region = var.region
	source_ami = data.amazon-parameterstore.base.value
	instance_type = var.instance_type
	subnet_id = var.subnet_id
	associate_public_ip_address = true
	temporary_security_group_source_cidrs = var.ssh_cidrs
	ssh_username = "ec2-user"

	launch_block_device_mappings {
		device_name = "/dev/xvda"
		volume_size = 30
		volume_type = "gp3"
		delete_on_termination = true
	}

	ami_name = "asyncdb-etcd-${local.timestamp}"
	ami_description = "The etcd tier's boot dependencies, including the etcd image and the AWS CLI it discovers its peers with"
	run_tags = { Name = "asyncdb-etcd-ami-builder", "asyncdb:run" = var.run_id }
	tags = merge(local.tags, { Name = "asyncdb-etcd-${local.timestamp}", "asyncdb:role" = "etcd" })
	snapshot_tags = merge(local.tags, { Name = "asyncdb-etcd-${local.timestamp}", "asyncdb:role" = "etcd" })
}

build {
	name = "database"
	sources = ["source.amazon-ebs.database"]

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/database.sh"
	}

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/awscli.sh"
	}

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/clean.sh"
	}

	post-processor "manifest" {
		output = "manifest.json"
		strip_path = true
	}
}

build {
	name = "etcd"
	sources = ["source.amazon-ebs.etcd"]

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/etcd.sh"
	}

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/awscli.sh"
	}

	provisioner "shell" {
		execute_command = "sudo -E bash '{{ .Path }}'"
		script = "${path.root}/clean.sh"
	}

	post-processor "manifest" {
		output = "manifest.json"
		strip_path = true
	}
}
